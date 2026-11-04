// Splits every n-gon into n quads: one new vertex at the face centroid and one at each edge midpoint, with no
// smoothing. Per-vertex attributes are dropped (the output has positions and connectivity only); face_group is
// inherited by the child faces. Vertex order is deterministic: the original vertices keep their indices, and the
// new ones follow in face order (each face's new edge midpoints, then its face point).
#include "fake_backend.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace rdesk::backends {

namespace {

constexpr std::size_t kMaxCorners = std::size_t{1} << 27;  // ~134M: refuse to run away

class FakeBackend final : public IRemeshBackend {
 public:
  FakeBackend() {
    info_.id = "fake.subdivide";
    info_.display_name = "Fake subdivider (test backend)";
    info_.version = "1";
    info_.expected_convention = Convention::RightHandedYUpCCW;
    info_.caps.progress = true;
    info_.caps.cooperative_cancel = true;
    info_.caps.deterministic = true;
    info_.caps.thread_safe = true;
    info_.caps.max_threads = 1;

    ParamSpec it;
    it.key = "iterations";
    it.label = "Iterations";
    it.group = "Subdivision";
    it.tooltip = "How many times every face is split into quads (0 returns the input unchanged).";
    it.type = ParamType::Int;
    it.default_value = std::int64_t{1};
    it.min = 0;
    it.max = 4;
    it.step = 1;
    info_.params.push_back(it);

    auto debug = [&](const char* key, const char* label, const char* tip, ParamType type, ParamValue def) {
      ParamSpec s;
      s.key = key;
      s.label = label;
      s.group = "Debug";
      s.tooltip = tip;
      s.type = type;
      s.default_value = std::move(def);
      s.advanced = true;
      info_.params.push_back(std::move(s));
    };
    debug("debug.sleep_ms_per_iter", "Sleep per iteration (ms)", "Slows every iteration down; cancel is still honoured.", ParamType::Int, std::int64_t{0});
    info_.params.back().min = 0;
    info_.params.back().max = 600000;
    debug("debug.fail", "Fail", "Return Errc::BackendFailed.", ParamType::Bool, false);
    debug("debug.crash", "Crash", "Write through a null pointer (the worker must report a crash, not hang).", ParamType::Bool, false);
    debug("debug.hang", "Hang", "Loop forever and ignore cancel; only a forced kill ends it.", ParamType::Bool, false);
    debug("debug.ignore_cancel", "Ignore cancel", "Run to completion even after a cancel request.", ParamType::Bool, false);
  }

  const BackendInfo& info() const override { return info_; }

  Result<RemeshOutput> remesh(const PolyMesh& in, const ParamMap& params, ProgressSink& sink) override {
    try {
      return run(in, params, sink);
    } catch (const std::exception& e) {
      return make_error(Errc::BackendFailed, std::string("fake.subdivide: ") + e.what());
    }
  }

 private:
  template <class T>
  static T get(const ParamMap& p, const char* key) {
    return std::get<T>(p.at(key));
  }

  // Sleeps `ms`, waking early on cancel unless `deaf`. Returns false if cancelled.
  static bool nap(std::int64_t ms, ProgressSink& sink, bool deaf) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      if (!deaf && sink.cancelled()) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
  }

  Result<RemeshOutput> run(const PolyMesh& in, const ParamMap& params, ProgressSink& sink) {
    const std::int64_t iterations = get<std::int64_t>(params, "iterations");
    const std::int64_t sleep_ms = get<std::int64_t>(params, "debug.sleep_ms_per_iter");
    const bool deaf = get<bool>(params, "debug.ignore_cancel");

    if (get<bool>(params, "debug.fail")) return make_error(Errc::BackendFailed, "fake.subdivide: failure requested by debug.fail");
    if (get<bool>(params, "debug.crash")) {
      volatile int* p = nullptr;
      *p = 1;  // deliberate: tests that the worker survives and reports a crashing backend
    }
    if (get<bool>(params, "debug.hang")) {
      sink.log(LogLevel::Warn, "debug.hang: ignoring cancel");
      for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sink.progress("subdivide", 0.0);
    PolyMesh cur = in;
    for (std::int64_t it = 0; it < iterations; ++it) {
      if (!deaf && sink.cancelled()) return make_error(Errc::Cancelled, "cancelled");
      if (sleep_ms > 0 && !nap(sleep_ms, sink, deaf)) return make_error(Errc::Cancelled, "cancelled");
      auto next = subdivide(cur, sink, deaf);
      if (!next) return next.error();
      cur = std::move(next).value();
      sink.progress("subdivide", static_cast<double>(it + 1) / static_cast<double>(iterations));
    }
    RemeshOutput out;
    out.mesh = std::move(cur);
    out.mesh.attrs = VertexAttributes{};
    out.backend_stats_json = "{\"iterations\":" + std::to_string(iterations) + "}";
    return out;
  }

  static Result<PolyMesh> subdivide(const PolyMesh& m, ProgressSink& sink, bool deaf) {
    std::size_t out_corners = 0;
    for (std::size_t f = 0; f < m.face_count(); ++f) out_corners += 4 * m.face_size(f);
    if (out_corners > kMaxCorners) return make_error(Errc::BackendFailed, "fake.subdivide: the result would be too large");

    PolyMesh r;
    r.convention = m.convention;
    r.positions = m.positions;
    r.face_offsets.reserve(m.corners.size() + 1);
    r.corners.reserve(out_corners);
    r.face_offsets.push_back(0);
    const bool grouped = !m.face_group.empty();
    std::unordered_map<std::uint64_t, std::uint32_t> mid;  // lookups only: iteration order never matters
    mid.reserve(m.corners.size());
    auto add_vertex = [&](double x, double y, double z) {
      const auto id = static_cast<std::uint32_t>(r.positions.size() / 3);
      r.positions.push_back(static_cast<float>(x));
      r.positions.push_back(static_cast<float>(y));
      r.positions.push_back(static_cast<float>(z));
      return id;
    };
    std::vector<std::uint32_t> edge_mid;
    for (std::size_t f = 0; f < m.face_count(); ++f) {
      if ((f & 4095u) == 0 && !deaf && sink.cancelled()) return make_error(Errc::Cancelled, "cancelled");
      const std::uint32_t b = m.face_offsets[f];
      const std::size_t n = m.face_size(f);
      const std::uint32_t* c = &m.corners[b];
      edge_mid.assign(n, 0);
      double cx = 0, cy = 0, cz = 0;
      for (std::size_t k = 0; k < n; ++k) {
        const std::uint32_t u = c[k], v = c[(k + 1) % n];
        const std::uint64_t key = (std::uint64_t{std::min(u, v)} << 32) | std::max(u, v);
        auto it = mid.find(key);
        if (it == mid.end()) {
          const float* pu = &m.positions[3 * std::size_t{u}];
          const float* pv = &m.positions[3 * std::size_t{v}];
          it = mid.emplace(key, add_vertex(0.5 * (double(pu[0]) + pv[0]), 0.5 * (double(pu[1]) + pv[1]), 0.5 * (double(pu[2]) + pv[2]))).first;
        }
        edge_mid[k] = it->second;
        const float* pk = &m.positions[3 * std::size_t{u}];
        cx += pk[0];
        cy += pk[1];
        cz += pk[2];
      }
      const double inv = 1.0 / static_cast<double>(n);
      const std::uint32_t fp = add_vertex(cx * inv, cy * inv, cz * inv);
      for (std::size_t k = 0; k < n; ++k) {
        // The quad at corner k: corner, next edge midpoint, face point, previous edge midpoint (same winding).
        r.corners.push_back(c[k]);
        r.corners.push_back(edge_mid[k]);
        r.corners.push_back(fp);
        r.corners.push_back(edge_mid[(k + n - 1) % n]);
        r.face_offsets.push_back(static_cast<std::uint32_t>(r.corners.size()));
        if (grouped) r.face_group.push_back(m.face_group[f]);
      }
    }
    return r;
  }

  BackendInfo info_;
};

}  // namespace

std::shared_ptr<IRemeshBackend> make_fake_backend() { return std::make_shared<FakeBackend>(); }

void register_fake_backend() { register_backend(make_fake_backend()); }

}  // namespace rdesk::backends
