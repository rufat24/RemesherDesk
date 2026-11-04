// rdesk: the command-line tool, and the worker process the GUI starts for every remesh job
// (docs/architecture.md §4). Subcommands: remesh, backends, info, compare, convert, --version.
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <crtdbg.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "events.hpp"
#include "exit_codes.hpp"
#include "fake_backend.hpp"
#include "rdesk/backend.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/json_codec.hpp"
#include "rdesk/metrics.hpp"
#include "rdesk/obj.hpp"
#include "rdesk/paths.hpp"
#include "rdesk/rdm.hpp"
#include "rdesk/sha256.hpp"
#include "rdesk/utf.hpp"
#include "rdesk/validate.hpp"
#include "rdesk/version.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace rdesk;
using namespace rdesk::cli;
using rdesk::platform::path_from_utf8;
using rdesk::platform::path_to_utf8;

namespace {

// ---------------------------------------------------------------------------------------- arguments

struct OptSpec {
  std::set<std::string> values;  // options that take a value: --backend X
  std::set<std::string> flags;   // options without a value: --json
  std::size_t min_positional = 0, max_positional = 0;
};

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
  const std::string* get(const std::string& k) const {
    const auto it = values.find(k);
    return it == values.end() ? nullptr : &it->second;
  }
};

Result<Args> parse_args(const std::vector<std::string>& in, const OptSpec& spec) {
  Args a;
  for (std::size_t i = 0; i < in.size(); ++i) {
    const std::string& s = in[i];
    if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
      if (spec.flags.count(s)) {
        a.flags.insert(s);
      } else if (spec.values.count(s)) {
        if (i + 1 >= in.size()) return make_error(Errc::Usage, "option " + s + " needs a value");
        a.values[s] = in[++i];
      } else {
        return make_error(Errc::Usage, "unknown option " + s);
      }
    } else {
      a.positional.push_back(s);
    }
  }
  if (a.positional.size() < spec.min_positional) return make_error(Errc::Usage, "missing argument");
  if (a.positional.size() > spec.max_positional) return make_error(Errc::Usage, "unexpected argument '" + a.positional[spec.max_positional] + "'");
  return a;
}

// ---------------------------------------------------------------------------------------- mesh files

enum class Format { Rdm, Obj };

Result<Format> format_of(const fs::path& p, Errc on_unknown) {
  std::string ext = path_to_utf8(p.extension());
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".rdm") return Format::Rdm;
  if (ext == ".obj") return Format::Obj;
  return make_error(on_unknown, "unsupported file type '" + ext + "' (expected .rdm or .obj): " + path_to_utf8(p));
}

Result<std::vector<std::uint8_t>> read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return make_error(Errc::IoRead, "cannot open " + path_to_utf8(p));
  in.seekg(0, std::ios::end);
  const auto len = in.tellg();
  if (len < 0) return make_error(Errc::IoRead, "cannot determine the size of " + path_to_utf8(p));
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(len));
  if (len > 0 && !in.read(reinterpret_cast<char*>(buf.data()), len)) return make_error(Errc::IoRead, "read error on " + path_to_utf8(p));
  return buf;
}

struct Loaded {
  PolyMesh mesh;
  std::string sha256;
  std::vector<std::string> warnings;
};

Result<Loaded> load_mesh(const fs::path& p, Convention obj_convention) {
  auto fmt = format_of(p, Errc::Parse);
  if (!fmt) return fmt.error();
  auto bytes = read_bytes(p);
  if (!bytes) return bytes.error();
  Loaded out;
  out.sha256 = sha256_hex(bytes.value().data(), bytes.value().size());
  if (fmt.value() == Format::Rdm) {
    auto r = io::read_rdm(bytes.value().data(), bytes.value().size());
    if (!r) return r.error();
    out.mesh = std::move(r.value().mesh);
    out.warnings = std::move(r.value().warnings);
  } else {
    io::ObjReadOptions o;
    o.source_convention = obj_convention;
    auto r = io::read_obj(std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()), o);
    if (!r) return r.error();
    out.mesh = std::move(r.value().mesh);
    out.warnings = std::move(r.value().warnings);
  }
  return out;
}

Result<void> save_mesh(const fs::path& p, const PolyMesh& m, const io::RdmWriteOptions& rdm_opts) {
  auto fmt = format_of(p, Errc::Usage);
  if (!fmt) return fmt.error();
  if (fmt.value() == Format::Rdm) return io::write_rdm_file(p, m, rdm_opts);
  return io::write_obj_file(p, m, {});
}

json counts(const PolyMesh& m) {
  std::size_t tri = 0, quad = 0, ngon = 0;
  for (std::size_t f = 0; f < m.face_count(); ++f) {
    const std::size_t n = m.face_size(f);
    if (n == 3) ++tri; else if (n == 4) ++quad; else if (n > 4) ++ngon;
  }
  return {{"v", m.vertex_count()}, {"f", m.face_count()}, {"tri", tri}, {"quad", quad}, {"ngon", ngon}};
}

Result<Convention> parse_convention(const std::string* s) {
  if (!s) return Convention::RightHandedYUpCCW;
  if (*s == "rh") return Convention::RightHandedYUpCCW;
  if (*s == "unity") return Convention::UnityLeftHandedYUpCW;
  return make_error(Errc::Usage, "--in-convention must be 'unity' or 'rh'");
}

bool stdout_is_tty() {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) != 0;
#else
  return isatty(STDOUT_FILENO) != 0;
#endif
}

void print_error(const Error& e) { std::fprintf(stderr, "error: %s\n", e.message.c_str()); }

// ---------------------------------------------------------------------------------------- remesh worker

std::atomic<bool> g_cancel{false};  // set by the stdin watcher; static so that the detached thread can never outlive it

void watch_stdin() {
  std::string line;
  while (std::getline(std::cin, line)) {
    json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_object() && j.value("t", "") == "cancel") g_cancel.store(true);
  }
  g_cancel.store(true);  // EOF: the parent is gone (or closed our stdin on purpose)
}

enum class Protocol { Jsonl, Text };

long long current_pid() {
#ifdef _WIN32
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(getpid());
#endif
}

class Reporter {
 public:
  explicit Reporter(Protocol p) : proto_(p), ev_(stdout) {}

  void hello(const std::string& backend, const BackendInfo* info) {
    if (proto_ == Protocol::Text) return;
    json j = {{"pid", current_pid()}, {"rdesk", std::string(version_string())}, {"backend", backend}};
    if (info) {
      j["backend_version"] = info->version;
      j["caps"] = {{"progress", info->caps.progress}, {"cooperative_cancel", info->caps.cooperative_cancel}, {"deterministic", info->caps.deterministic}};
    }
    ev_.emit("hello", std::move(j));
  }

  void progress(const std::string& stage, std::optional<double> frac) {
    std::lock_guard<std::mutex> lock(m_);
    const auto now = std::chrono::steady_clock::now();
    json j = {{"stage", stage}};
    if (frac) {
      last_frac_ = std::max(last_frac_, std::min(1.0, std::max(0.0, *frac)));  // never decreases
      j["frac"] = last_frac_;
    } else {
      j["frac"] = nullptr;
    }
    const bool stage_changed = stage != last_stage_;
    const bool due = (now - last_emit_) >= std::chrono::milliseconds(50);  // at most 20 per second
    if (!(stage_changed || due || (frac && *frac >= 1.0))) return;
    last_stage_ = stage;
    last_emit_ = now;
    if (proto_ == Protocol::Jsonl) ev_.emit("progress", std::move(j));
    else std::printf("[%3d%%] %s\n", static_cast<int>(last_frac_ * 100.0), stage.c_str()), std::fflush(stdout);
  }

  void log(LogLevel level, std::string_view msg) {
    if (proto_ == Protocol::Jsonl) ev_.emit("log", {{"level", to_string(level)}, {"msg", std::string(msg)}});
    else std::fprintf(stderr, "%s: %.*s\n", to_string(level), static_cast<int>(msg.size()), msg.data());
  }

  int succeed(json result, const fs::path& result_json) {
    if (proto_ == Protocol::Jsonl) ev_.emit("result", result);
    else std::printf("ok: wrote %s (sha256 %s, %lld ms)\n", result["out"].get<std::string>().c_str(), result["output_sha256"].get<std::string>().c_str(),
                     static_cast<long long>(result["stats"]["ms"]["total"].get<long long>()));
    write_result_json(result_json, {{"ok", true}, {"result", result}});
    return bye(kExitOk);
  }

  int fail(const Error& e, const fs::path& result_json = {}) {
    json err = {{"code", error_name(e.code)}, {"msg", e.message}};
    if (!e.detail_json.empty()) {
      json d = json::parse(e.detail_json, nullptr, false);
      if (!d.is_discarded()) err["detail"] = std::move(d);
    }
    if (proto_ == Protocol::Jsonl) ev_.emit("error", err);
    else std::fprintf(stderr, "error: %s\n", e.message.c_str());
    write_result_json(result_json, {{"ok", false}, {"error", err}});
    return bye(exit_code_for(e.code));
  }

  long long elapsed_ms() const { return ev_.elapsed_ms(); }

 private:
  int bye(int code) {
    if (proto_ == Protocol::Jsonl) ev_.emit("bye", {{"exit", code}});
    return code;
  }
  static void write_result_json(const fs::path& p, const json& j) {
    if (p.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(p.parent_path(), ec)) return;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (o) o << j.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
  }

  Protocol proto_;
  EventWriter ev_;
  std::mutex m_;
  double last_frac_ = 0.0;
  std::string last_stage_;
  std::chrono::steady_clock::time_point last_emit_{};
};

// Progress of the whole job: load 0..5 %, remesh 5..90 %, write 90..100 %.
class WorkerSink final : public ProgressSink {
 public:
  explicit WorkerSink(Reporter& r) : rep_(r) {}
  void progress(std::string_view stage, std::optional<double> fraction) override {
    if (fraction) rep_.progress(std::string(stage), 0.05 + 0.85 * std::min(1.0, std::max(0.0, *fraction)));
    else rep_.progress(std::string(stage), std::nullopt);
  }
  void log(LogLevel l, std::string_view msg) override { rep_.log(l, msg); }
  bool cancelled() const noexcept override { return g_cancel.load(std::memory_order_relaxed); }

 private:
  Reporter& rep_;
};

long long ms_between(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

int cmd_remesh(const std::vector<std::string>& argv) {
  OptSpec spec;
  spec.values = {"--backend", "--in", "--out", "--params", "--protocol", "--threads", "--in-convention"};
  spec.max_positional = 0;
  auto parsed = parse_args(argv, spec);

  Protocol proto = stdout_is_tty() ? Protocol::Text : Protocol::Jsonl;
  if (parsed) {
    if (const auto* p = parsed.value().get("--protocol")) {
      if (*p == "jsonl") proto = Protocol::Jsonl;
      else if (*p == "text") proto = Protocol::Text;
      else parsed = make_error(Errc::Usage, "--protocol must be 'jsonl' or 'text'");
    }
  }
  Reporter rep(proto);
  const std::string backend_id = parsed && parsed.value().get("--backend") ? *parsed.value().get("--backend") : std::string();
  const auto backend = backend_id.empty() ? nullptr : find_backend(backend_id);
  rep.hello(backend_id, backend ? &backend->info() : nullptr);  // always the first event, whatever happens next

  fs::path result_json;
  try {
    if (!parsed) return rep.fail(parsed.error());
    const Args& a = parsed.value();
    for (const char* req : {"--backend", "--in", "--out"}) {
      if (!a.get(req)) return rep.fail(make_error(Errc::Usage, std::string("missing ") + req));
    }
    const fs::path in_path = path_from_utf8(*a.get("--in")), out_path = path_from_utf8(*a.get("--out"));
    result_json = out_path.parent_path() / "result.json";
    if (!backend) return rep.fail(make_error(Errc::BackendUnavailable, "no backend with id '" + backend_id + "'"), result_json);
    auto conv = parse_convention(a.get("--in-convention"));
    if (!conv) return rep.fail(conv.error(), result_json);

    if (proto == Protocol::Jsonl) std::thread(watch_stdin).detach();  // text mode (a human, maybe with stdin closed) never cancels this way

    const auto t0 = std::chrono::steady_clock::now();
    rep.progress("load", 0.0);
    auto loaded = load_mesh(in_path, conv.value());
    if (!loaded) return rep.fail(loaded.error(), result_json);
    for (const auto& w : loaded.value().warnings) rep.log(LogLevel::Warn, w);
    const auto t_loaded = std::chrono::steady_clock::now();
    rep.progress("load", 0.05);

    ParamMap params;
    if (const auto* pf = a.get("--params")) {
      auto bytes = read_bytes(path_from_utf8(*pf));
      if (!bytes) return rep.fail(bytes.error(), result_json);
      auto pm = io::params_from_json(std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
      if (!pm) return rep.fail(pm.error(), result_json);
      params = std::move(pm).value();
    }
    // Everything not mentioned takes its default; this is also what gets recorded in the output.
    auto full_params = complete_params(backend->info().params, params);
    if (!full_params) return rep.fail(full_params.error(), result_json);

    WorkerSink sink(rep);
    auto out = remesh(backend_id, loaded.value().mesh, params, sink);
    if (!out) return rep.fail(out.error(), result_json);
    const auto t_remeshed = std::chrono::steady_clock::now();
    rep.progress("write", 0.9);

    io::RdmWriteOptions wo;
    wo.producer = "rdesk " + std::string(version_string());
    json meta = {{"backend", backend->info().id},
                 {"backend_version", backend->info().version},
                 {"input_sha256", loaded.value().sha256},
                 {"params", json::parse(io::params_to_json(full_params.value()))}};
    wo.extra_meta_json = meta.dump();
    if (auto w = save_mesh(out_path, out.value().mesh, wo); !w) return rep.fail(w.error(), result_json);
    auto written = read_bytes(out_path);
    if (!written) return rep.fail(make_error(Errc::IoWrite, "cannot read back the output: " + written.error().message), result_json);
    const auto t_done = std::chrono::steady_clock::now();
    rep.progress("write", 1.0);

    json result = {{"out", path_to_utf8(out_path)},
                   {"output_sha256", sha256_hex(written.value().data(), written.value().size())},
                   {"stats",
                    {{"in", counts(loaded.value().mesh)},
                     {"out", counts(out.value().mesh)},
                     {"ms", {{"load", ms_between(t0, t_loaded)}, {"remesh", ms_between(t_loaded, t_remeshed)}, {"write", ms_between(t_remeshed, t_done)}, {"total", ms_between(t0, t_done)}}}}}};
    if (!out.value().backend_stats_json.empty()) {
      json bs = json::parse(out.value().backend_stats_json, nullptr, false);
      if (!bs.is_discarded()) result["backend"] = std::move(bs);
    }
    return rep.succeed(std::move(result), result_json);
  } catch (const std::exception& e) {
    return rep.fail(make_error(Errc::Internal, std::string("unexpected failure: ") + e.what()), result_json);
  } catch (...) {
    return rep.fail(make_error(Errc::Internal, "unexpected failure"), result_json);
  }
}

// ---------------------------------------------------------------------------------------- other commands

int cmd_backends(const std::vector<std::string>& argv) {
  OptSpec spec;
  spec.flags = {"--json"};
  auto a = parse_args(argv, spec);
  if (!a) return print_error(a.error()), exit_code_for(a.error().code);
  const auto all = registered_backends();
  if (a.value().flags.count("--json")) {
    json arr = json::array();
    for (const auto& b : all) arr.push_back(json::parse(io::backend_info_to_json(b->info())));
    std::printf("%s\n", arr.dump().c_str());
  } else {
    for (const auto& b : all) std::printf("%s  %s (version %s, %zu parameters)\n", b->info().id.c_str(), b->info().display_name.c_str(), b->info().version.c_str(), b->info().params.size());
  }
  return kExitOk;
}

json validation_json(const ValidationReport& v) {
  json j = {{"errors", v.has_errors()},
            {"closed_manifold", v.is_closed_manifold()},
            {"zero_area_faces", v.zero_area_faces.count},
            {"repeated_vertex_faces", v.repeated_vertex_faces.count},
            {"unreferenced_vertices", v.unreferenced_vertices},
            {"nonfinite_vertices", v.nonfinite_vertices.count},
            {"faces_too_small", v.faces_too_small.count},
            {"index_out_of_range_faces", v.index_out_of_range_faces.count}};
  if (!v.format_errors.empty()) j["format_errors"] = v.format_errors;
  if (v.topology_analyzed) {
    j["components"] = v.topology.component_count;
    j["boundary_loops"] = v.topology.boundary_loops;
    j["boundary_edges"] = v.topology.boundary_edges;
    j["euler"] = v.topology.euler;
    j["nonmanifold_edges"] = v.topology.nonmanifold_edges.count;
    j["nonmanifold_vertices"] = v.topology.nonmanifold_vertices.count;
    j["orientation_inconsistent_edges"] = v.topology.orientation_inconsistent_edges.count;
  }
  return j;
}

int cmd_info(const std::vector<std::string>& argv) {
  OptSpec spec;
  spec.flags = {"--json"};
  spec.values = {"--in-convention"};
  spec.min_positional = spec.max_positional = 1;
  auto a = parse_args(argv, spec);
  if (!a) return print_error(a.error()), exit_code_for(a.error().code);
  auto conv = parse_convention(a.value().get("--in-convention"));
  if (!conv) return print_error(conv.error()), exit_code_for(conv.error().code);
  auto m = load_mesh(path_from_utf8(a.value().positional[0]), conv.value());
  if (!m) return print_error(m.error()), exit_code_for(m.error().code);
  const PolyMesh& mesh = m.value().mesh;
  const ValidationReport v = validate(mesh);
  json j = {{"path", a.value().positional[0]}, {"sha256", m.value().sha256}, {"convention", to_string(mesh.convention)}, {"counts", counts(mesh)}, {"validation", validation_json(v)}};
  if (auto s = compute_stats(mesh)) {
    const MeshStats& st = s.value();
    j["stats"] = {{"quad_ratio", st.quad_ratio}, {"unique_edges", st.unique_edges}, {"irregular_vertices", st.irregular_vertices}, {"surface_area", st.surface_area},
                  {"edge_length", {{"min", st.edge_length.min}, {"mean", st.edge_length.mean}, {"max", st.edge_length.max}}}};
    if (st.bbox.valid) j["bbox"] = {{"min", st.bbox.min}, {"max", st.bbox.max}, {"diagonal", st.bbox.diagonal()}};
  }
  if (a.value().flags.count("--json")) {
    std::printf("%s\n", j.dump(2).c_str());
  } else {
    std::printf("%s\n  convention: %s\n  vertices: %zu  faces: %zu (tri %zu, quad %zu, ngon %zu)\n  closed manifold: %s  components: %zu  boundary loops: %zu\n",
                a.value().positional[0].c_str(), to_string(mesh.convention), mesh.vertex_count(), mesh.face_count(), j["counts"]["tri"].get<std::size_t>(),
                j["counts"]["quad"].get<std::size_t>(), j["counts"]["ngon"].get<std::size_t>(), v.is_closed_manifold() ? "yes" : "no",
                v.topology.component_count, v.topology.boundary_loops);
    if (v.has_errors()) std::printf("  INVALID: see --json for details\n");
    for (const auto& w : m.value().warnings) std::printf("  warning: %s\n", w.c_str());
  }
  return v.has_errors() ? kExitInvalid : kExitOk;
}

int cmd_compare(const std::vector<std::string>& argv) {
  OptSpec spec;
  spec.flags = {"--json"};
  spec.values = {"--tier", "--in-convention"};
  spec.min_positional = spec.max_positional = 2;
  auto a = parse_args(argv, spec);
  if (!a) return print_error(a.error()), exit_code_for(a.error().code);
  Tier tier = Tier::T2;
  if (const auto* t = a.value().get("--tier")) {
    if (*t == "T0") tier = Tier::T0; else if (*t == "T1") tier = Tier::T1; else if (*t == "T2") tier = Tier::T2;
    else return std::fprintf(stderr, "error: --tier must be T0, T1 or T2\n"), kExitUsage;
  }
  auto conv = parse_convention(a.value().get("--in-convention"));
  if (!conv) return print_error(conv.error()), exit_code_for(conv.error().code);
  auto ref = load_mesh(path_from_utf8(a.value().positional[0]), conv.value());
  if (!ref) return print_error(ref.error()), exit_code_for(ref.error().code);
  auto test = load_mesh(path_from_utf8(a.value().positional[1]), conv.value());
  if (!test) return print_error(test.error()), exit_code_for(test.error().code);
  convert_in_place(test.value().mesh, ref.value().mesh.convention);  // compare in one convention
  auto rep = compare(ref.value().mesh, test.value().mesh, tier);
  if (!rep) return print_error(rep.error()), exit_code_for(rep.error().code);
  if (a.value().flags.count("--json")) {
    std::printf("%s\n", rep.value().to_json().c_str());
  } else {
    std::printf("%s: %s\n", to_string(rep.value().tier), rep.value().pass ? "PASS" : "FAIL");
    for (const Check& c : rep.value().checks) std::printf("  %-40s %-12.6g limit %-12.6g %s\n", c.name.c_str(), c.value, c.limit, c.pass ? "ok" : "FAIL");
  }
  return rep.value().pass ? kExitOk : 1;  // 1: a clean "not equal" verdict, distinct from the error codes
}

int cmd_convert(const std::vector<std::string>& argv) {
  OptSpec spec;
  spec.values = {"--in-convention"};
  spec.min_positional = spec.max_positional = 2;
  auto a = parse_args(argv, spec);
  if (!a) return print_error(a.error()), exit_code_for(a.error().code);
  auto conv = parse_convention(a.value().get("--in-convention"));
  if (!conv) return print_error(conv.error()), exit_code_for(conv.error().code);
  auto m = load_mesh(path_from_utf8(a.value().positional[0]), conv.value());
  if (!m) return print_error(m.error()), exit_code_for(m.error().code);
  io::RdmWriteOptions wo;
  wo.producer = "rdesk " + std::string(version_string());
  if (auto w = save_mesh(path_from_utf8(a.value().positional[1]), m.value().mesh, wo); !w) return print_error(w.error()), exit_code_for(w.error().code);
  for (const auto& w : m.value().warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
  return kExitOk;
}

void usage(std::FILE* to) {
  std::fputs(
      "usage: rdesk <command> [options]\n"
      "  remesh --backend ID --in FILE --out FILE [--params FILE] [--protocol jsonl|text] [--threads N] [--in-convention unity|rh]\n"
      "  backends [--json]\n"
      "  info FILE [--json] [--in-convention unity|rh]\n"
      "  compare REF TEST [--tier T0|T1|T2] [--json]\n"
      "  convert IN OUT [--in-convention unity|rh]      (.rdm <-> .obj)\n"
      "  --version\n"
      "exit codes (remesh): 0 ok, 2 usage, 3 input unreadable, 4 input/parameters invalid, 5 backend failed, 6 cancelled,\n"
      "                     7 backend unavailable, 8 output write error, 70 internal error. compare: 1 = verdict FAIL.\n",
      to);
}

int real_main(const std::vector<std::string>& args) {
  backends::register_fake_backend();
  if (args.empty()) return usage(stderr), kExitUsage;
  const std::string& cmd = args[0];
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  try {
    if (cmd == "--version" || cmd == "-V" || cmd == "version") {
      std::printf("rdesk %.*s\n", static_cast<int>(version_string().size()), version_string().data());
      return kExitOk;
    }
    if (cmd == "--help" || cmd == "-h" || cmd == "help") return usage(stdout), kExitOk;
    if (cmd == "remesh") return cmd_remesh(rest);
    if (cmd == "backends") return cmd_backends(rest);
    if (cmd == "info") return cmd_info(rest);
    if (cmd == "compare") return cmd_compare(rest);
    if (cmd == "convert") return cmd_convert(rest);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "internal error: %s\n", e.what());
    return kExitInternal;
  }
  std::fprintf(stderr, "error: unknown command '%s'\n", cmd.c_str());
  usage(stderr);
  return kExitUsage;
}

// Ends the process without running exit-time cleanup. The stdin watcher thread may be blocked in a read that holds the
// stdio lock for stdin; exit() would try to take that lock while flushing and wait until the parent sends or closes
// something. Everything we wrote has been flushed by now.
[[noreturn]] void finish(int code) {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(code);
}

void harden_process() {
#ifdef _WIN32
  // A crash must end the process, not open an error-reporting dialog that nobody can click in a headless worker.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _setmode(_fileno(stdout), _O_BINARY);  // "\n" stays "\n"
  _setmode(_fileno(stdin), _O_BINARY);
#endif
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  harden_process();
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    auto u = platform::wide_to_utf8(argv[i]);
    if (!u) {
      std::fprintf(stderr, "error: argument %d is not valid UTF-16\n", i);
      return kExitUsage;
    }
    args.push_back(std::move(u).value());
  }
  finish(real_main(args));
}
#else
int main(int argc, char** argv) {
  harden_process();
  finish(real_main(std::vector<std::string>(argv + 1, argv + argc)));
}
#endif
