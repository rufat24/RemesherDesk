// Worker protocol conformance (docs/architecture.md §4). Drives the real `rdesk` executable through
// rdesk::platform::Process, the same way the GUI will.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "doctest/doctest.h"
#include "fixtures.hpp"
#include "rdesk/obj.hpp"
#include "rdesk/paths.hpp"
#include "rdesk/process.hpp"
#include "rdesk/rdm.hpp"
#include "rdesk/sha256.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;
using namespace rdesk::platform;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Run {
  std::vector<json> events;
  std::vector<std::string> raw_lines;
  std::optional<ExitStatus> status;
  std::string stderr_text;
  double wall_ms = 0;
  bool killed = false;
};

using OnEvent = std::function<void(Process&, const json&)>;

// Starts rdesk, feeds events to `on_event` (which may write to stdin or kill), and collects everything until the
// stream ends. Gives up after `timeout_ms` and kills the worker.
Run run_rdesk(std::vector<std::string> args, const OnEvent& on_event = {}, int timeout_ms = 30000) {
  ProcessOptions o;
  o.exe = RDESK_EXE;
  o.args = std::move(args);
  auto sp = Process::spawn(o);
  REQUIRE_MESSAGE(sp.ok(), (sp.ok() ? "" : sp.error().message));
  auto p = std::move(sp).value();
  Run run;
  const auto t0 = Clock::now();
  std::string line;
  for (;;) {
    const int left = timeout_ms - static_cast<int>(ms_since(t0));
    if (left <= 0) {
      p->kill();
      run.killed = true;
      break;
    }
    const LineStatus st = p->read_stdout_line(line, left);
    if (st == LineStatus::Eof) break;
    if (st == LineStatus::Timeout) continue;
    run.raw_lines.push_back(line);
    json j = json::parse(line, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
      run.events.push_back(j);
      if (on_event) on_event(*p, j);
    }
  }
  run.status = p->wait(10000);
  if (!run.status) {
    p->kill();
    run.status = p->wait(5000);
  }
  run.wall_ms = ms_since(t0);
  run.stderr_text = p->stderr_text();
  return run;
}

struct Job {
  fs::path dir;
  explicit Job(const std::string& name = "job") {
    static int counter = 0;
    dir = fs::temp_directory_path() / path_from_utf8("rdesk_proto_" + std::to_string(++counter) + "_" + name);  // name is UTF-8
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~Job() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path in() const { return dir / "input.rdm"; }
  fs::path out() const { return dir / "output.rdm"; }
  void write_cube() const { REQUIRE(io::write_rdm_file(in(), fixtures::make_cube()).ok()); }
  void write_params(const std::string& text) const { std::ofstream(dir / "params.json") << text; }
  std::vector<std::string> remesh_args(const std::string& backend = "fake.subdivide", bool with_params = false) const {
    std::vector<std::string> a = {"remesh", "--backend", backend, "--in", path_to_utf8(in()), "--out", path_to_utf8(out()), "--protocol", "jsonl"};
    if (with_params) {
      a.push_back("--params");
      a.push_back(path_to_utf8(dir / "params.json"));
    }
    return a;
  }
};

const json* find_event(const Run& r, const char* type) {
  for (const json& e : r.events) if (e.value("t", "") == type) return &e;
  return nullptr;
}
std::size_t count_events(const Run& r, const char* type) {
  return static_cast<std::size_t>(std::count_if(r.events.begin(), r.events.end(), [&](const json& e) { return e.value("t", "") == type; }));
}
int exit_of(const Run& r) { return r.status && r.status->exited_normally ? r.status->code : -1; }

// Framing rules every run must satisfy, whatever the outcome.
void check_framing(const Run& r) {
  REQUIRE(r.events.size() >= 2);
  CHECK(r.raw_lines.size() == r.events.size());  // every stdout line is a JSON object
  CHECK(r.events.front().value("t", "") == "hello");
  CHECK(r.events.back().value("t", "") == "bye");
  long long last_ms = -1;
  double last_frac = -1;
  for (const json& e : r.events) {
    CHECK(e.value("v", 0) == 1);
    CHECK(e.contains("t"));
    REQUIRE(e.contains("ms"));
    CHECK(e["ms"].get<long long>() >= last_ms);
    last_ms = e["ms"].get<long long>();
    if (e.value("t", "") == "progress" && e.contains("frac") && e["frac"].is_number()) {
      CHECK(e["frac"].get<double>() >= last_frac);  // progress never decreases
      last_frac = e["frac"].get<double>();
    }
  }
  CHECK(count_events(r, "hello") == 1);
  CHECK(count_events(r, "bye") == 1);
  CHECK(count_events(r, "result") + count_events(r, "error") == 1);  // exactly one outcome
  CHECK(r.events.back().value("exit", -999) == exit_of(r));          // bye announces the real exit code
}

int trials() {
  const auto v = fixtures::env_value("RDESK_PROTOCOL_TRIALS");
  return v ? std::max(1, std::atoi(v->c_str())) : 20;
}

}  // namespace

TEST_CASE("protocol: a successful job") {
  Job job;
  job.write_cube();
  job.write_params(R"({"iterations":1})");
  const Run r = run_rdesk(job.remesh_args("fake.subdivide", true));
  REQUIRE(r.status.has_value());
  CHECK(exit_of(r) == 0);
  check_framing(r);

  const json* hello = find_event(r, "hello");
  REQUIRE(hello);
  CHECK(hello->value("backend", "") == "fake.subdivide");
  CHECK(hello->contains("pid"));
  CHECK((*hello)["caps"]["cooperative_cancel"].get<bool>());

  const json* res = find_event(r, "result");
  REQUIRE(res);
  CHECK(fs::exists(job.out()));
  // output_sha256 is the hash of the file as written.
  std::ifstream f(job.out(), std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(res->value("output_sha256", "") == sha256_hex(bytes.data(), bytes.size()));
  CHECK((*res)["stats"]["in"]["f"].get<int>() == 6);
  CHECK((*res)["stats"]["out"]["f"].get<int>() == 24);
  CHECK((*res)["stats"]["ms"].contains("total"));

  // The output is a valid RDM with provenance in META, and it is a closed quad mesh.
  auto back = io::read_rdm_file(job.out());
  REQUIRE(back.ok());
  CHECK(back.value().mesh.face_count() == 24);
  const json meta = json::parse(back.value().meta_json);
  CHECK(meta["backend"] == "fake.subdivide");
  CHECK(meta["params"]["iterations"] == 1);
  CHECK(meta["input_sha256"].get<std::string>().size() == 64);
  CHECK(validate(back.value().mesh).is_closed_manifold());

  // result.json next to the output mirrors the result event.
  std::ifstream rj(job.dir / "result.json");
  REQUIRE(rj.good());
  const json result_file = json::parse(rj);
  CHECK(result_file["ok"] == true);
  CHECK(result_file["result"]["output_sha256"] == res->value("output_sha256", ""));
}

TEST_CASE("protocol: progress is monotonic and rate-limited on a longer job") {
  Job job;
  REQUIRE(io::write_rdm_file(job.in(), fixtures::make_grid(40, 40)).ok());
  job.write_params(R"({"iterations":3,"debug.sleep_ms_per_iter":30})");
  const Run r = run_rdesk(job.remesh_args("fake.subdivide", true));
  CHECK(exit_of(r) == 0);
  check_framing(r);
  const std::size_t n = count_events(r, "progress");
  CHECK(n >= 3);
  CHECK(static_cast<double>(n) / std::max(1.0, r.wall_ms / 1000.0) <= 25.0);  // about 20 per second at most
  CHECK(find_event(r, "progress") != nullptr);
}

TEST_CASE("protocol: exit codes") {
  struct Expect {
    const char* name;
    std::function<Run(Job&)> go;
    int code;
    const char* error_code;
  };
  const std::vector<Expect> cases = {
      {"missing --backend", [](Job& j) { j.write_cube(); return run_rdesk({"remesh", "--in", path_to_utf8(j.in()), "--out", path_to_utf8(j.out()), "--protocol", "jsonl"}); }, 2, "E_USAGE"},
      {"unknown option", [](Job& j) { j.write_cube(); auto a = j.remesh_args(); a.push_back("--bogus"); return run_rdesk(a); }, 2, "E_USAGE"},
      {"bad --protocol", [](Job& j) { j.write_cube(); auto a = j.remesh_args(); a.back() = "carrier-pigeon"; return run_rdesk(a); }, 2, "E_USAGE"},
      {"unsupported output type", [](Job& j) { j.write_cube(); auto a = j.remesh_args(); a[6] = path_to_utf8(j.dir / "out.txt"); return run_rdesk(a); }, 2, "E_USAGE"},
      {"missing input file", [](Job& j) { return run_rdesk(j.remesh_args()); }, 3, "E_IO_READ"},
      {"corrupt input file", [](Job& j) { std::ofstream(j.in(), std::ios::binary) << "this is not a mesh"; return run_rdesk(j.remesh_args()); }, 3, "E_PARSE"},
      {"mesh without faces", [](Job& j) { PolyMesh m; m.face_offsets = {0}; REQUIRE(io::write_rdm_file(j.in(), m).ok()); return run_rdesk(j.remesh_args()); }, 4, "E_INPUT_INVALID"},
      {"parameter out of range", [](Job& j) { j.write_cube(); j.write_params(R"({"iterations":99})"); return run_rdesk(j.remesh_args("fake.subdivide", true)); }, 4, "E_PARAM_INVALID"},
      {"unknown parameter", [](Job& j) { j.write_cube(); j.write_params(R"({"nope":1})"); return run_rdesk(j.remesh_args("fake.subdivide", true)); }, 4, "E_PARAM_INVALID"},
      {"params file is not JSON", [](Job& j) { j.write_cube(); j.write_params("{oops"); return run_rdesk(j.remesh_args("fake.subdivide", true)); }, 4, "E_PARAM_INVALID"},
      {"backend failure", [](Job& j) { j.write_cube(); j.write_params(R"({"debug.fail":true})"); return run_rdesk(j.remesh_args("fake.subdivide", true)); }, 5, "E_BACKEND_FAILED"},
      {"unknown backend", [](Job& j) { j.write_cube(); return run_rdesk(j.remesh_args("no.such.backend")); }, 7, "E_BACKEND_UNAVAILABLE"},
      {"output cannot be written", [](Job& j) { j.write_cube(); auto a = j.remesh_args(); a[6] = path_to_utf8(j.dir / "no_such_dir" / "o.rdm"); return run_rdesk(a); }, 8, "E_IO_WRITE"},
  };
  for (const Expect& c : cases) {
    INFO("case: " << c.name);
    Job job;
    const Run r = c.go(job);
    REQUIRE(r.status.has_value());
    CHECK(exit_of(r) == c.code);
    check_framing(r);  // hello first and bye last even when everything goes wrong
    const json* err = find_event(r, "error");
    REQUIRE(err);
    CHECK(err->value("code", "") == c.error_code);
    CHECK_FALSE(err->value("msg", "").empty());
  }
}

TEST_CASE("protocol: a parameter error names the key in the detail") {
  Job job;
  job.write_cube();
  job.write_params(R"({"iterations":99})");
  const Run r = run_rdesk(job.remesh_args("fake.subdivide", true));
  const json* err = find_event(r, "error");
  REQUIRE(err);
  CHECK((*err)["detail"]["key"] == "iterations");
  CHECK(err->value("msg", "").find("iterations") != std::string::npos);
  std::ifstream rj(job.dir / "result.json");
  CHECK(json::parse(rj)["ok"] == false);
}

TEST_CASE("protocol: cooperative cancel exits 6 and the UI is idle again within 1.25 s (20 trials)") {
  Job job;
  job.write_cube();
  job.write_params(R"({"iterations":4,"debug.sleep_ms_per_iter":60000})");
  double worst = 0;
  for (int t = 0; t < trials(); ++t) {
    Clock::time_point clicked;
    bool sent = false;
    Run r = run_rdesk(job.remesh_args("fake.subdivide", true), [&](Process& p, const json& e) {
      if (!sent && e.value("t", "") == "progress" && e.value("stage", "") == "subdivide") {  // the backend is running
        sent = true;
        clicked = Clock::now();
        p.write_stdin("{\"t\":\"cancel\"}\n");
      }
    });
    const double idle_ms = ms_since(clicked);  // the harness has seen the process end by now
    REQUIRE(sent);
    worst = std::max(worst, idle_ms);
    INFO("trial " << t << ": " << idle_ms << " ms after the click");
    CHECK(exit_of(r) == 6);
    CHECK(idle_ms <= 1250.0);
    const json* err = find_event(r, "error");
    REQUIRE(err);
    CHECK(err->value("code", "") == "E_CANCELLED");
    check_framing(r);
    CHECK_FALSE(fs::exists(job.out()));  // no partial output
  }
  std::cout << "[protocol] cooperative cancel: worst " << worst << " ms over " << trials() << " trials\n";
}

TEST_CASE("protocol: a hanging backend is force-killed after the grace period (20 trials)") {
  Job job;
  job.write_cube();
  job.write_params(R"({"debug.hang":true})");
  const int kGraceMs = 1000;
  double worst = 0;
  for (int t = 0; t < trials(); ++t) {
    bool killed = false;
    // The generic runner only reads events; the grace/kill policy needs control of the process, so drive it by hand.
    ProcessOptions o;
    o.exe = RDESK_EXE;
    o.args = job.remesh_args("fake.subdivide", true);
    auto sp = Process::spawn(o);
    REQUIRE(sp.ok());
    auto p = std::move(sp).value();
    std::string line;
    bool hanging = false;
    const auto t0 = Clock::now();
    while (!hanging && ms_since(t0) < 20000) {
      if (p->read_stdout_line(line, 100) == LineStatus::Line) {
        const json e = json::parse(line, nullptr, false);
        hanging = e.is_object() && e.value("t", "") == "log";  // "debug.hang: ignoring cancel"
      }
    }
    REQUIRE(hanging);
    const auto clicked = Clock::now();
    REQUIRE(p->write_stdin("{\"t\":\"cancel\"}\n"));
    auto st = p->wait(kGraceMs);
    if (!st) {  // the UI policy: still alive after the grace period -> forced kill
      p->kill();
      killed = true;
      st = p->wait(2000);
    }
    const double idle_ms = ms_since(clicked);
    worst = std::max(worst, idle_ms);
    INFO("trial " << t << ": " << idle_ms << " ms after the click");
    REQUIRE(st.has_value());
    CHECK(killed);  // the backend ignores cancel, so only the kill can end it
    CHECK(st->killed_by_us);
    CHECK(idle_ms <= 1250.0);
  }
  std::cout << "[protocol] hang + forced kill: worst " << worst << " ms over " << trials() << " trials\n";
}

TEST_CASE("protocol: a crashing backend ends the process without hanging") {
  Job job;
  job.write_cube();
  job.write_params(R"({"debug.crash":true})");
  const auto t0 = Clock::now();
  const Run r = run_rdesk(job.remesh_args("fake.subdivide", true), {}, 10000);
  REQUIRE(r.status.has_value());
  CHECK_FALSE(r.killed);                      // it ended by itself: no error-reporting dialog stalled it
  CHECK(ms_since(t0) < 10000.0);
  // A crash is anything that is not one of the documented exit codes (under ASan a null write exits with 1).
  const std::set<int> documented = {0, 2, 3, 4, 5, 6, 7, 8, 70};
  const bool documented_exit = r.status->exited_normally && documented.count(r.status->code);
  CHECK_FALSE(documented_exit);
  CHECK(find_event(r, "hello") != nullptr);
  CHECK(find_event(r, "bye") == nullptr);     // it never got to say goodbye
  CHECK(find_event(r, "result") == nullptr);
  CHECK_FALSE(fs::exists(job.out()));
}

TEST_CASE("protocol: EOF on stdin is a cancel") {
  Job job;
  job.write_cube();
  job.write_params(R"({"iterations":4,"debug.sleep_ms_per_iter":60000})");
  Clock::time_point closed;
  bool done = false;
  const Run r = run_rdesk(job.remesh_args("fake.subdivide", true), [&](Process& p, const json& e) {
    if (!done && e.value("t", "") == "progress" && e.value("stage", "") == "subdivide") {
      done = true;
      closed = Clock::now();
      p.close_stdin();  // the parent "died"
    }
  });
  REQUIRE(done);
  CHECK(exit_of(r) == 6);
  CHECK(ms_since(closed) <= 1250.0);
  CHECK(find_event(r, "error")->value("code", "") == "E_CANCELLED");
}

TEST_CASE("protocol: --protocol text prints for humans, not JSON") {
  Job job;
  job.write_cube();
  auto args = job.remesh_args();
  args.back() = "text";
  ProcessOptions o;
  o.exe = RDESK_EXE;
  o.args = args;
  auto sp = Process::spawn(o);
  REQUIRE(sp.ok());
  auto p = std::move(sp).value();
  std::string line, all;
  while (p->read_stdout_line(line, 10000) == LineStatus::Line) all += line + "\n";
  const auto st = p->wait(10000);
  REQUIRE(st.has_value());
  CHECK(st->code == 0);
  CHECK(all.find("ok: wrote") != std::string::npos);
  CHECK(all.find("\"t\":") == std::string::npos);
  CHECK(fs::exists(job.out()));
}

TEST_CASE("protocol: non-ASCII paths work end to end") {
  Job job("j\xC3\xB6" "b_\xE6\x97\xA5\xE6\x9C\xAC");  // jöb_日本 (split so that "b" is not read as a hex digit)
  job.write_cube();
  const Run r = run_rdesk(job.remesh_args());
  CHECK(exit_of(r) == 0);
  check_framing(r);
  CHECK(fs::exists(job.out()));
  CHECK(find_event(r, "result")->value("out", "") == path_to_utf8(job.out()));
}

TEST_CASE("cli: backends, info, compare, convert, version") {
  Job job;
  job.write_cube();

  SUBCASE("version and its start-up cost") {
    std::vector<double> times;
    for (int i = 0; i < 7; ++i) {
      const auto t0 = Clock::now();
      const Run r = run_rdesk({"--version"});
      times.push_back(ms_since(t0));
      CHECK(exit_of(r) == 0);
      REQUIRE(r.raw_lines.size() == 1);
      CHECK(r.raw_lines[0].rfind("rdesk ", 0) == 0);
    }
    std::sort(times.begin(), times.end());
    std::cout << "[protocol] rdesk --version round trip: median " << times[times.size() / 2] << " ms, max " << times.back() << " ms\n";
    CHECK(times[times.size() / 2] < 2000.0);
  }
  SUBCASE("backends --json describes every parameter") {
    const Run r = run_rdesk({"backends", "--json"});
    REQUIRE(exit_of(r) == 0);
    REQUIRE(r.raw_lines.size() == 1);
    const json arr = json::parse(r.raw_lines[0]);
    REQUIRE(arr.is_array());
    const auto it = std::find_if(arr.begin(), arr.end(), [](const json& b) { return b["id"] == "fake.subdivide"; });
    REQUIRE(it != arr.end());
    bool found = false;
    for (const json& p : (*it)["params"]) {
      if (p["key"] == "iterations") {
        found = true;
        CHECK(p["min"] == 0);
        CHECK(p["max"] == 4);
        CHECK(p["default"] == 1);
        CHECK(p["type"] == "int");
      }
    }
    CHECK(found);
  }
  SUBCASE("info --json") {
    const Run r = run_rdesk({"info", path_to_utf8(job.in()), "--json"});
    REQUIRE(exit_of(r) == 0);
    std::string all;  // --json prints indented JSON over several lines
    for (const auto& l : r.raw_lines) all += l + "\n";
    const json j = json::parse(all);
    CHECK(j["counts"]["f"] == 6);
    CHECK(j["validation"]["closed_manifold"] == true);
    CHECK(j["validation"]["euler"] == 2);
  }
  SUBCASE("convert .rdm -> .obj -> .rdm keeps the mesh bit for bit, and compare says so") {
    const auto obj = path_to_utf8(job.dir / "m.obj"), back = path_to_utf8(job.dir / "back.rdm");
    REQUIRE(exit_of(run_rdesk({"convert", path_to_utf8(job.in()), obj})) == 0);
    REQUIRE(exit_of(run_rdesk({"convert", obj, back})) == 0);
    CHECK(exit_of(run_rdesk({"compare", path_to_utf8(job.in()), back, "--tier", "T0"})) == 0);
    const Run bad = run_rdesk({"compare", path_to_utf8(job.in()), "x.rdm"});
    CHECK(exit_of(bad) == 3);
  }
  SUBCASE("compare reports a verdict with exit code 1") {
    REQUIRE(exit_of(run_rdesk(job.remesh_args())) == 0);
    const Run r = run_rdesk({"compare", path_to_utf8(job.in()), path_to_utf8(job.out()), "--tier", "T0", "--json"});
    CHECK(exit_of(r) == 1);
    REQUIRE(r.raw_lines.size() == 1);
    CHECK(json::parse(r.raw_lines[0])["pass"] == false);
  }
  SUBCASE("usage errors") {
    CHECK(exit_of(run_rdesk({})) == 2);
    CHECK(exit_of(run_rdesk({"frobnicate"})) == 2);
    CHECK(exit_of(run_rdesk({"info"})) == 2);
    CHECK(exit_of(run_rdesk({"compare", "only-one"})) == 2);
  }
}
