#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#include "doctest/doctest.h"
#include "rdesk/paths.hpp"
#include "rdesk/process.hpp"
#include "rdesk/utf.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace rdesk;
using namespace rdesk::platform;
using Clock = std::chrono::steady_clock;

namespace {

ProcessOptions child(std::vector<std::string> args) {
  ProcessOptions o;
  o.exe = RDESK_TEST_CHILD;
  o.args = std::move(args);
  return o;
}

std::unique_ptr<Process> start(std::vector<std::string> args) {
  auto r = Process::spawn(child(std::move(args)));
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  return std::move(r).value();
}

std::vector<std::string> read_all_lines(Process& p, int timeout_ms = 10000) {
  std::vector<std::string> out;
  std::string line;
  for (;;) {
    const LineStatus st = p.read_stdout_line(line, timeout_ms);
    if (st != LineStatus::Line) break;
    out.push_back(line);
  }
  return out;
}

// True while the process exists and is not a zombie.
bool alive(std::int64_t pid) {
#ifdef _WIN32
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return running;
#else
  std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
  if (!f) return false;
  std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto close_paren = all.rfind(')');
  return close_paren != std::string::npos && close_paren + 2 < all.size() && all[close_paren + 2] != 'Z';
#endif
}

bool wait_until_dead(std::int64_t pid, int ms) {
  const auto end = Clock::now() + std::chrono::milliseconds(ms);
  while (Clock::now() < end) {
    if (!alive(pid)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return !alive(pid);
}

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

}  // namespace

TEST_CASE("process: arguments reach the child intact, including spaces, quotes, backslashes and non-ASCII text") {
  const std::vector<std::string> args = {
      "plain",
      "two words",
      "h\xC3\xA9llo w\xC3\xB6rld \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x99\x82",  // héllo wörld 日本語 🙂
      "say \"hi\"",
      "C:\\path with space\\dir\\",
      "ends with backslashes\\\\",
      "\\\"mixed\\\\\"",
      "",
      "tab\there",
      "percent %PATH% and $HOME and *?",
  };
  std::vector<std::string> cmd = {"args"};
  cmd.insert(cmd.end(), args.begin(), args.end());
  auto p = start(cmd);
  const auto lines = read_all_lines(*p);
  CHECK(lines == args);
  const auto st = p->wait(5000);
  REQUIRE(st.has_value());
  CHECK(st->exited_normally);
  CHECK(st->code == 0);
  CHECK(st->describe() == "exit 0");
}

TEST_CASE("process: stdin to stdout round trip, CRLF is stripped, EOF on stdin ends the child") {
  auto p = start({"echo"});
  REQUIRE(p->write_stdin("first\n"));
  std::string line;
  REQUIRE(p->read_stdout_line(line, 5000) == LineStatus::Line);
  CHECK(line == "first");
  REQUIRE(p->write_stdin("with cr\r\n"));
  REQUIRE(p->read_stdout_line(line, 5000) == LineStatus::Line);
  CHECK(line == "with cr");
  REQUIRE(p->write_stdin("{\"t\":\"cancel\"}\n"));
  REQUIRE(p->read_stdout_line(line, 5000) == LineStatus::Line);
  CHECK(line == "{\"t\":\"cancel\"}");
  p->close_stdin();
  REQUIRE(p->read_stdout_line(line, 5000) == LineStatus::Line);
  CHECK(line == "eof");
  CHECK(p->read_stdout_line(line, 5000) == LineStatus::Eof);
  const auto st = p->wait(5000);
  REQUIRE(st.has_value());
  CHECK(st->code == 0);
}

TEST_CASE("process: stdin EOF alone ends a child that is waiting for input") {
  auto p = start({"eof"});
  CHECK_FALSE(p->wait(150).has_value());  // still waiting for input
  p->close_stdin();
  const auto st = p->wait(5000);
  REQUIRE(st.has_value());
  CHECK(st->code == 17);
}

TEST_CASE("process: a line delivered in two pieces is returned whole, and a timeout is not an error") {
  auto p = start({"partial"});
  std::string line;
  CHECK(p->read_stdout_line(line, 30) == LineStatus::Timeout);  // "ab" has no newline yet
  REQUIRE(p->read_stdout_line(line, 5000) == LineStatus::Line);
  CHECK(line == "abcd");
  CHECK(p->read_stdout_line(line, 5000) == LineStatus::Eof);
}

TEST_CASE("process: exit codes and stderr") {
  {
    auto p = start({"exit", "6"});
    const auto st = p->wait(5000);
    REQUIRE(st.has_value());
    CHECK(st->exited_normally);
    CHECK(st->code == 6);
    CHECK_FALSE(st->crashed);
    CHECK_FALSE(st->killed_by_us);
  }
  {
    auto p = start({"stderr", "something went wrong\n"});
    const auto st = p->wait(5000);
    REQUIRE(st.has_value());
    CHECK(st->code == 3);
    // The reader thread may lag the exit slightly.
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (p->stderr_text().empty() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(p->stderr_text() == "something went wrong\n");
  }
}

TEST_CASE("process: a burst of output larger than the queue is neither lost nor a deadlock (backpressure)") {
  auto p = start({"flood", "200000"});
  const auto lines = read_all_lines(*p, 20000);
  REQUIRE(lines.size() == 200000);
  CHECK(lines.front() == "line 0");
  CHECK(lines.back() == "line 199999");
}

TEST_CASE("process: kill ends a sleeping child within 200 ms") {
  auto p = start({"sleep", "60000"});
  CHECK_FALSE(p->try_wait().has_value());
  const std::int64_t pid = p->pid();
  CHECK(alive(pid));
  const auto t0 = Clock::now();
  p->kill();
  const auto st = p->wait(2000);
  const double ms = ms_since(t0);
  REQUIRE(st.has_value());
  CHECK_MESSAGE(ms <= 200.0, "took " << ms << " ms");
  CHECK(st->killed_by_us);
  CHECK(st->describe() == "killed");
  CHECK_FALSE(alive(pid));
  p->kill();  // idempotent
}

TEST_CASE("process: killing the child also kills what it started") {
  auto p = start({"grandchild", "60000"});
  std::string line;
  REQUIRE(p->read_stdout_line(line, 10000) == LineStatus::Line);
  const std::int64_t grandchild = std::stoll(line);
  CHECK(alive(grandchild));
  p->kill();
  REQUIRE(p->wait(5000).has_value());
  CHECK_MESSAGE(wait_until_dead(grandchild, 5000), "grandchild " << grandchild << " survived");
  // Both pipes are closed again, so the reader sees the end of the stream.
  CHECK(p->read_stdout_line(line, 5000) == LineStatus::Eof);
}

TEST_CASE("process: destroying the object kills a running child") {
  std::int64_t pid;
  {
    auto p = start({"sleep", "60000"});
    pid = p->pid();
    REQUIRE(alive(pid));
  }
  CHECK(wait_until_dead(pid, 5000));
}

TEST_CASE("process: a crash is reported as a crash, quickly, and does not hang") {
  const auto t0 = Clock::now();
  auto p = start({"crash"});
  const auto st = p->wait(10000);
  REQUIRE(st.has_value());
  CHECK_MESSAGE(ms_since(t0) < 10000.0, "no error-reporting dialog may stall the exit");
  CHECK(st->crashed);
  CHECK_FALSE(st->exited_normally);
  CHECK_FALSE(st->killed_by_us);
#ifdef _WIN32
  CHECK(st->raw == 0xC0000005u);
  CHECK(st->describe() == "crashed (0xC0000005)");
#else
  CHECK(st->signal == SIGABRT);
  CHECK(st->describe() == "crashed (signal 6)");
#endif
}

TEST_CASE("process: starting something that does not exist is BackendUnavailable") {
  ProcessOptions o;
  o.exe = std::filesystem::path(RDESK_TEST_CHILD).parent_path() / "no_such_program_here";
  auto r = Process::spawn(o);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == Errc::BackendUnavailable);
  CHECK(r.error().message.find("no_such_program_here") != std::string::npos);

  ProcessOptions dir;
  dir.exe = std::filesystem::path(RDESK_TEST_CHILD).parent_path();  // a directory
  CHECK(Process::spawn(dir).error().code == Errc::BackendUnavailable);
}

TEST_CASE("process: only the three pipes are inherited") {
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;  // inheritable, but not on the handle list
  HANDLE h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  auto p = start({"handle", std::to_string(reinterpret_cast<std::uintptr_t>(h))});
  const auto lines = read_all_lines(*p);
  CloseHandle(h);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "closed");
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
  // Descriptors without close-on-exec must not leak into the child.
  const int leak1 = ::open("/dev/null", O_RDONLY);
  const int leak2 = ::open("/dev/null", O_RDONLY);
  REQUIRE(leak1 >= 0);
  REQUIRE(leak2 >= 0);
  auto p = start({"fds"});
  const auto lines = read_all_lines(*p);
  ::close(leak1);
  ::close(leak2);
  std::set<std::string> fds(lines.begin(), lines.end());
  CHECK(fds == std::set<std::string>{"0", "1", "2"});
#else
  MESSAGE("descriptor inheritance is only enforced with glibc >= 2.34 or on Windows");
#endif
}

TEST_CASE("process: abandon (the parent 'dies') ends the child on Windows; elsewhere it is a no-op") {
  auto p = start({"sleep", "60000"});
  const std::int64_t pid = p->pid();
  p->abandon();
#ifdef _WIN32
  CHECK(wait_until_dead(pid, 5000));
#else
  CHECK(alive(pid));
#endif
}

TEST_CASE("utf: validation and round trip") {
  CHECK(is_valid_utf8("plain ascii"));
  CHECK(is_valid_utf8("h\xC3\xA9llo \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x99\x82"));
  CHECK_FALSE(is_valid_utf8("\xC3"));              // truncated
  CHECK_FALSE(is_valid_utf8("\xC0\xAF"));          // overlong '/'
  CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));      // a surrogate
  CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // above U+10FFFF
  CHECK_FALSE(is_valid_utf8("\xFF"));
  const std::string text = "h\xC3\xA9llo \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x99\x82";
  auto w = utf8_to_wide(text);
  REQUIRE(w.ok());
  auto back = wide_to_utf8(w.value());
  REQUIRE(back.ok());
  CHECK(back.value() == text);
  CHECK_FALSE(utf8_to_wide("\xC3").ok());
  CHECK(utf8_to_wide("").ok());
}

TEST_CASE("paths: job folders are unique, named like yyyymmdd-hhmmss-xxxx, and pruned oldest first") {
  const auto root = std::filesystem::temp_directory_path() / "rdesk_test_jobs";
  std::filesystem::remove_all(root);
  std::set<std::filesystem::path> made;
  for (int i = 0; i < 25; ++i) {
    auto d = make_job_dir(root);
    REQUIRE(d.ok());
    CHECK(std::filesystem::is_directory(d.value()));
    CHECK(d.value().filename().string().size() == 20);
    made.insert(d.value());
  }
  CHECK(made.size() == 25);

  // A folder that does not look like a job must survive pruning.
  std::filesystem::create_directories(root / "not-a-job");
  std::ofstream(root / "stray.txt") << "x";
  // Two old jobs, named so that they sort first.
  std::filesystem::create_directories(root / "20000101-000000-0000");
  std::filesystem::create_directories(root / "20000101-000001-0001");
  CHECK(prune_job_dirs(root, 10) == 17);  // 27 job folders -> 10 left
  std::size_t jobs = 0;
  for (const auto& e : std::filesystem::directory_iterator(root)) {
    const std::string n = e.path().filename().string();
    if (n.size() == 20 && n[8] == '-') ++jobs;
  }
  CHECK(jobs == 10);
  CHECK_FALSE(std::filesystem::exists(root / "20000101-000000-0000"));
  CHECK(std::filesystem::exists(root / "not-a-job"));
  CHECK(std::filesystem::exists(root / "stray.txt"));
  CHECK(prune_job_dirs(root, 10) == 0);
  CHECK(prune_job_dirs(root / "missing", 10) == 0);
  std::filesystem::remove_all(root);
}

TEST_CASE("paths: the executable and the default jobs root") {
  auto exe = executable_path();
  REQUIRE(exe.ok());
  CHECK(std::filesystem::exists(exe.value()));
  CHECK(exe.value().is_absolute());
  auto root = default_jobs_root();
  if (root.ok()) {
    CHECK(root.value().filename() == "jobs");
    CHECK(root.value().is_absolute());
  }
}

TEST_CASE("paths: UTF-8 path conversion round trips non-ASCII names") {
  const std::string name = "d\xC3\xA9j\xC3\xA0 \xE6\x97\xA5\xE6\x9C\xAC.rdm";
  const auto p = path_from_utf8(name);
  CHECK_FALSE(p.empty());
  CHECK(path_to_utf8(p) == name);
  CHECK(path_to_utf8(path_from_utf8("plain/dir/file.obj")) == std::string("plain") + std::filesystem::path::preferred_separator + "dir" +
                                                                    std::filesystem::path::preferred_separator + "file.obj");
}
