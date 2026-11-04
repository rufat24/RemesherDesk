#include "rdesk/paths.hpp"
#include "rdesk/utf.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rdesk::platform {

namespace fs = std::filesystem;

fs::path path_from_utf8(std::string_view utf8) {
#ifdef _WIN32
  auto w = utf8_to_wide(utf8);
  return w ? fs::path(w.value()) : fs::path();
#else
  return fs::path(std::string(utf8));
#endif
}

std::string path_to_utf8(const fs::path& p) {
#ifdef _WIN32
  auto u = wide_to_utf8(p.wstring());
  return u ? u.value() : std::string();
#else
  return p.string();
#endif
}

Result<fs::path> executable_path() {
#ifdef _WIN32
  std::wstring buf(1024, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return make_error(Errc::Internal, "GetModuleFileNameW failed");
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf);
    }
    buf.resize(buf.size() * 2);
  }
#else
  std::error_code ec;
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return make_error(Errc::Internal, "cannot read /proc/self/exe: " + ec.message());
  return p;
#endif
}

namespace {
std::string env_utf8(const char* name) {
#ifdef _WIN32
  char* buf = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return {};
  std::string v(buf);
  std::free(buf);
  return v;
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool matches_job_pattern(const std::string& n) {  // yyyymmdd-hhmmss-xxxx
  if (n.size() != 20 || n[8] != '-' || n[15] != '-') return false;
  for (std::size_t i = 0; i < 8; ++i) if (!is_digit(n[i])) return false;
  for (std::size_t i = 9; i < 15; ++i) if (!is_digit(n[i])) return false;
  for (std::size_t i = 16; i < 20; ++i) if (!is_hex(n[i])) return false;
  return true;
}
}  // namespace

Result<fs::path> default_jobs_root() {
#ifdef _WIN32
  const std::string base = env_utf8("LOCALAPPDATA");
  if (base.empty()) return make_error(Errc::Internal, "%LOCALAPPDATA% is not set");
  return fs::path(base) / "RemesherDesk" / "jobs";
#else
  std::string base = env_utf8("XDG_STATE_HOME");
  if (!base.empty()) return fs::path(base) / "remesherdesk" / "jobs";
  const std::string home = env_utf8("HOME");
  if (home.empty()) return make_error(Errc::Internal, "neither $XDG_STATE_HOME nor $HOME is set");
  return fs::path(home) / ".local" / "state" / "remesherdesk" / "jobs";
#endif
}

Result<fs::path> make_job_dir(const fs::path& root) {
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) return make_error(Errc::IoWrite, "cannot create " + root.string() + ": " + ec.message());
  std::random_device rd;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char name[96];  // roomy enough for any int the compiler cannot rule out
    std::snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d-%04x", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, static_cast<unsigned>(rd() & 0xFFFFu));
    const fs::path dir = root / name;
    if (fs::create_directory(dir, ec)) return dir;
    if (ec) return make_error(Errc::IoWrite, "cannot create " + dir.string() + ": " + ec.message());
  }
  return make_error(Errc::IoWrite, "cannot find an unused job folder name under " + root.string());
}

std::size_t prune_job_dirs(const fs::path& root, std::size_t keep) {
  std::error_code ec;
  std::vector<fs::path> jobs;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_directory(e2) && !e2 && matches_job_pattern(it->path().filename().string())) jobs.push_back(it->path());
  }
  if (jobs.size() <= keep) return 0;
  std::sort(jobs.begin(), jobs.end());  // timestamp order == name order
  std::size_t removed = 0;
  for (std::size_t i = 0; i + keep < jobs.size(); ++i) {
    std::error_code e3;
    fs::remove_all(jobs[i], e3);
    if (!e3) ++removed;
  }
  return removed;
}

}  // namespace rdesk::platform
