#include <algorithm>
#include <exception>
#include <mutex>

#include "rdesk/backend.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/validate.hpp"

namespace rdesk {

const char* to_string(LogLevel l) noexcept {
  switch (l) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "info";
}

// ------------------------------------------------------------------------------------------ registry

namespace {
struct Registry {
  std::mutex mutex;
  std::vector<std::shared_ptr<IRemeshBackend>> items;
};
Registry& registry() {
  static Registry r;  // function-local: initialised on first use, no static-init-order problem
  return r;
}
}  // namespace

void register_backend(std::shared_ptr<IRemeshBackend> backend) {
  if (!backend) return;
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const std::string id = backend->info().id;
  auto it = std::find_if(r.items.begin(), r.items.end(), [&](const auto& b) { return b->info().id == id; });
  if (it != r.items.end()) *it = std::move(backend);
  else r.items.push_back(std::move(backend));
}

bool unregister_backend(std::string_view id) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = std::find_if(r.items.begin(), r.items.end(), [&](const auto& b) { return b->info().id == id; });
  if (it == r.items.end()) return false;
  r.items.erase(it);
  return true;
}

std::vector<std::shared_ptr<IRemeshBackend>> registered_backends() {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.items;
}

std::shared_ptr<IRemeshBackend> find_backend(std::string_view id) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = std::find_if(r.items.begin(), r.items.end(), [&](const auto& b) { return b->info().id == id; });
  return it == r.items.end() ? nullptr : *it;
}

// ------------------------------------------------------------------------------------------ facade

namespace {

std::string summary_json(const ValidationReport& v) {
  return "{\"index_out_of_range_faces\":" + std::to_string(v.index_out_of_range_faces.count) +
         ",\"faces_too_small\":" + std::to_string(v.faces_too_small.count) +
         ",\"nonfinite_vertices\":" + std::to_string(v.nonfinite_vertices.count) +
         ",\"format_errors\":" + std::to_string(v.format_errors.size()) + "}";
}

Result<void> check_input(const PolyMesh& in, const BackendInfo& info) {
  const ValidationReport v = validate(in);
  if (v.has_errors()) {
    std::string why = !v.format_errors.empty() ? v.format_errors.front()
                      : v.index_out_of_range_faces.count ? "a face refers to a vertex that does not exist"
                      : v.faces_too_small.count          ? "a face has fewer than 3 corners"
                                                         : "a vertex position is not finite";
    return make_error(Errc::InputInvalid, "input mesh is invalid: " + why, summary_json(v));
  }
  if (v.face_count == 0) return make_error(Errc::InputInvalid, "input mesh has no faces");
  const InputRequirements& req = info.requirements;
  if (req.triangles_only) {
    for (std::size_t f = 0; f < in.face_count(); ++f) {
      if (in.face_size(f) != 3) {
        return make_error(Errc::InputInvalid, "backend '" + info.id + "' needs a triangle mesh; face " + std::to_string(f) + " has " +
                                                  std::to_string(in.face_size(f)) + " corners");
      }
    }
  }
  if (req.require_closed && v.topology.boundary_edges != 0) {
    return make_error(Errc::InputInvalid, "backend '" + info.id + "' needs a closed mesh; found " + std::to_string(v.topology.boundary_edges) +
                                              " boundary edge(s)");
  }
  if (req.require_manifold && (v.topology.nonmanifold_edges.count || v.topology.nonmanifold_vertices.count ||
                               v.topology.orientation_inconsistent_edges.count)) {
    return make_error(Errc::InputInvalid,
                      "backend '" + info.id + "' needs a manifold, consistently oriented mesh; found " +
                          std::to_string(v.topology.nonmanifold_edges.count) + " non-manifold edge(s), " +
                          std::to_string(v.topology.nonmanifold_vertices.count) + " non-manifold vertex(es), " +
                          std::to_string(v.topology.orientation_inconsistent_edges.count) + " inconsistently oriented edge(s)");
  }
  return {};
}

}  // namespace

Result<RemeshOutput> remesh(std::string_view backend_id, const PolyMesh& in, const ParamMap& params, ProgressSink& sink) {
  const std::shared_ptr<IRemeshBackend> backend = find_backend(backend_id);
  if (!backend) return make_error(Errc::BackendUnavailable, "no backend with id '" + std::string(backend_id) + "'");
  const BackendInfo& info = backend->info();

  if (auto r = check_input(in, info); !r) return r.error();
  auto full = complete_params(info.params, params);
  if (!full) return full.error();
  if (sink.cancelled()) return make_error(Errc::Cancelled, "cancelled before the backend started");

  const Convention original = in.convention;
  PolyMesh prepared = to_convention(in, info.expected_convention);

  Result<RemeshOutput> out = make_error(Errc::Internal, "backend returned nothing");
  try {
    out = backend->remesh(prepared, full.value(), sink);
  } catch (const std::exception& e) {
    return make_error(Errc::BackendFailed, std::string("backend '") + info.id + "' threw: " + e.what());
  } catch (...) {
    return make_error(Errc::BackendFailed, "backend '" + info.id + "' threw an unknown exception");
  }
  if (!out) return out.error();

  RemeshOutput result = std::move(out).value();
  result.mesh.convention = info.expected_convention;  // the contract: the backend answers in its own convention
  if (sink.cancelled()) return make_error(Errc::Cancelled, "cancelled");
  const ValidationReport v = validate(result.mesh);
  if (v.has_errors()) {
    return make_error(Errc::OutputInvalid, "backend '" + info.id + "' produced an invalid mesh", summary_json(v));
  }
  convert_in_place(result.mesh, original);
  return result;
}

}  // namespace rdesk
