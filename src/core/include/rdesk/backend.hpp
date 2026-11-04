// Remesher backend interface, registry and the facade (docs/architecture.md §3).
// Nothing here knows whether a backend is compiled source, a prebuilt DLL or a port.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rdesk/mesh.hpp"
#include "rdesk/params.hpp"
#include "rdesk/result.hpp"

namespace rdesk {

enum class LogLevel { Debug, Info, Warn, Error };
const char* to_string(LogLevel l) noexcept;

struct Capabilities {
  bool progress = false;            // reports fractions
  bool cooperative_cancel = false;  // polls ProgressSink::cancelled()
  bool deterministic = false;       // same input + params + threads -> identical output bytes
  bool thread_safe = false;         // may run concurrently in one process (the worker never relies on it)
  int max_threads = 1;              // 0 = the backend decides
};

// Conditions the facade checks on the input before the backend sees it (Errc::InputInvalid).
struct InputRequirements {
  bool require_manifold = false;  // no non-manifold edge or vertex, consistent orientation
  bool require_closed = false;    // no boundary edge
  bool triangles_only = false;
};

struct BackendInfo {
  std::string id;  // "fake.subdivide", "quadremesh.src", ...
  std::string display_name, version;
  Convention expected_convention = Convention::RightHandedYUpCCW;
  Capabilities caps;
  InputRequirements requirements;
  std::vector<ParamSpec> params;
};

// Implemented by the worker (JSON lines) and by tests.
class ProgressSink {
 public:
  virtual ~ProgressSink() = default;
  virtual void progress(std::string_view stage, std::optional<double> fraction) = 0;
  virtual void log(LogLevel level, std::string_view message) = 0;
  // Cheap atomic load. Cooperative backends poll at least every 100 ms.
  virtual bool cancelled() const noexcept = 0;
};

// A sink that ignores progress and logs and has a cancel flag.
class NullProgressSink : public ProgressSink {
 public:
  void progress(std::string_view, std::optional<double>) override {}
  void log(LogLevel, std::string_view) override {}
  bool cancelled() const noexcept override { return cancel_.load(std::memory_order_relaxed); }
  void request_cancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }

 private:
  std::atomic<bool> cancel_{false};
};

struct RemeshOutput {
  PolyMesh mesh;
  std::string backend_stats_json;  // optional JSON object text
};

class IRemeshBackend {
 public:
  virtual ~IRemeshBackend() = default;
  virtual const BackendInfo& info() const = 0;
  // `in` is already validated, in info().expected_convention, and `params` is complete (defaults filled,
  // ranges checked). The result is in info().expected_convention. Must not throw: adapters catch everything
  // and return Errc::BackendFailed / Internal; return Errc::Cancelled after sink.cancelled() turned true.
  virtual Result<RemeshOutput> remesh(const PolyMesh& in, const ParamMap& params, ProgressSink& sink) = 0;
};

// ---- registry. Backends are registered explicitly (static initialisers in static libraries get dropped by
// linkers); each backend library exposes a register_*() function that the executable calls.
// Registering an id that already exists replaces it. Thread-safe.
void register_backend(std::shared_ptr<IRemeshBackend> backend);
bool unregister_backend(std::string_view id);
std::vector<std::shared_ptr<IRemeshBackend>> registered_backends();
std::shared_ptr<IRemeshBackend> find_backend(std::string_view id);

// ---- facade: the only entry point the CLI calls.
//  1. find the backend                                        -> BackendUnavailable
//  2. validate the input (structure + the backend's requirements) -> InputInvalid
//  3. complete and check the parameters                        -> ParamInvalid
//  4. convert to the backend's convention, call it, convert the result back to the input's convention
//  5. validate the output (structure, finite)                  -> OutputInvalid
// Cancellation before the call, or reported by the backend, gives Cancelled.
Result<RemeshOutput> remesh(std::string_view backend_id, const PolyMesh& in, const ParamMap& params, ProgressSink& sink);

}  // namespace rdesk
