# Architecture

Status: design. Implemented so far in `src/core`: `result.hpp`, `mesh.hpp`, `convention.hpp`, `validate.hpp` (task A2); in `src/io`: `rdm.hpp` (task A3). Everything marked
**(B)** depends on intake (`docs/intake.md`). Everything else can be built now.

## 1. Repository layout (target)

```
CMakeLists.txt  CMakePresets.json  cmake/          build system (verified: configures/builds/tests with gcc 13, clang 18)
src/core/        rdesk_core      data model, Result/Error, validation, conventions, metrics, backend interface, facade
src/io/          rdesk_io        RDM, OBJ (tinyobjloader), later PLY / glTF import
src/platform/    rdesk_platform  process spawn/kill (Win32 Job Objects, POSIX), paths, UTF-8 <-> UTF-16
src/backends/fake/               deterministic test backend (always built)
src/backends/quadremesh_src/     (B, branch S) adapter over vendored source
src/backends/quadremesh_dll/     (B, branch D) Windows-only LoadLibrary adapter
src/backends/oss_*/              (B, branch L only) open-source fallback backend
src/cli/         rdesk           command-line tool AND the worker process
src/app/         RemesherDesk    GUI (Polyscope/ImGui) — never links a backend
tests/unit/ tests/protocol/ tests/parity/ tests/fuzz/ tests/data/ tests/golden/ (B)
third_party/     pinned deps (FetchContent cache or vendored); third_party/quadremesh/ = untouched upstream (B)
tools/intake/    Inventory.ps1 (+ tests)       tools/unity/  MeshDump.cs (+ tests)     tools/rdm/  rdm.py
docs/
```

Dependency rules. A CMake target-link review enforces them; a test target `arch.deps` greps the
`target_link_libraries` calls.

- `core` depends on nothing but the standard library.
- `io` → `core`. `platform` → nothing. `backends/*` → `core` only.
- `cli` → `core`, `io`, `platform`, all backends compiled for the platform.
- `app` → `core`, `io`, `platform`, GUI deps. **It must not link backends.** It runs them through the worker,
  so a crash or a hang inside native code cannot take the GUI down.

## 2. Data model (`rdesk/mesh.hpp`)

```cpp
namespace rdesk {
enum class Convention : uint8_t { RightHandedYUpCCW, UnityLeftHandedYUpCW };

struct VertexAttributes {                 // all optional; sizes are multiples of vertex_count
  std::vector<float> normals;             // 3/vertex
  std::vector<float> tangents;            // 4/vertex
  std::vector<float> colors;              // 4/vertex
  std::array<std::vector<float>, 8> uv;   // uv_dim[i] floats/vertex
  std::array<uint8_t, 8> uv_dim{};        // 0 = absent, else 2..4
};

struct PolyMesh {
  std::vector<float>    positions;        // xyz, float32 — Unity's native precision
  std::vector<uint32_t> face_offsets;     // size F+1, face_offsets[0]==0 (CSR)
  std::vector<uint32_t> corners;          // vertex index per face corner
  std::vector<uint32_t> face_group;       // empty, or size F: submesh/material id
  VertexAttributes      attrs;            // per-vertex (Unity's split-vertex model)
  Convention            convention = Convention::RightHandedYUpCCW;

  size_t vertex_count() const { return positions.size() / 3; }
  size_t face_count()   const { return face_offsets.empty() ? 0 : face_offsets.size() - 1; }
};
}
```

Why this shape:

- **float32 positions.** The source data is float32. Storing doubles would make every round trip lossy or
  ambiguous and break bitwise parity. Backends convert internally if they want doubles.
- **CSR polygons with uint32 indices.** Inputs can be triangles, quads or n-gons. Unity `UInt16` meshes are
  widened on read. Meshes above 2^32−1 corners are a non-goal.
- **Per-vertex attributes**, like Unity: OBJ's per-corner `vt`/`vn` are split into unique (v, vt, vn) vertices on
  load. Remeshers generally need a welded surface, so `weld_by_position(mesh, eps)` exists (exact
  equality by default). **(B)** Intake decides whether the Quadremesh C# side welds before the native call. The
  adapter must reproduce exactly what C# does.
- **Units.** None are stored. Unity meters and OBJ numbers pass through unscaled. The UI shows
  the bounding-box size.
- **Conventions.** The internal canonical convention is right-handed, Y-up, CCW front faces, as in OBJ
  and the viewer. A backend declares the convention it expects. The facade converts at the adapter
  boundary (exact: X negation + winding reversal that keeps corner 0; see `docs/rdm-format.md`).
  So the Quadremesh adapter receives data bit-identical to what Unity passed.

Validation (`validate(const PolyMesh&) -> ValidationReport`) checks:

- index bounds
- face size ≥ 3
- repeated vertices within a face
- non-finite coordinates
- zero-area faces (area ≤ 1e-12·d², d = bbox diagonal)
- non-manifold edges (more than two incident faces)
- non-manifold vertices (fan count > 1)
- boundary loops
- connected components
- Euler characteristic and genus (for closed manifold components)
- orientation consistency (each interior edge used once in each direction)

It reports counts and the first N offending elements, so the GUI can highlight them.

## 3. Core facade and backend adapter API (`rdesk/backend.hpp`)

This is ABI-agnostic: nothing here knows whether a backend is compiled source, a prebuilt DLL or a port.

```cpp
namespace rdesk {
// ---- errors (no exceptions cross module boundaries)
enum class Errc { Usage, IoRead, Parse, InputInvalid, ParamInvalid, BackendUnavailable,
                  BackendFailed, Cancelled, IoWrite, OutputInvalid, Internal };
struct Error { Errc code; std::string message; std::string detail_json; };
template <class T> class Result;   // minimal expected<T, Error>; std::expected is NOT usable:
                                   // verified clang 18 + libstdc++ 13 cannot compile <expected> even with -std=c++23.

// ---- parameters: the GUI panel and the CLI --params file are generated from this
enum class ParamType { Bool, Int, Float, Enum, String };
using ParamValue = std::variant<bool, int64_t, double, std::string>;
struct ParamSpec {
  std::string key;           // stable id, e.g. "target_face_count" (B: mirrors the C# field name)
  std::string label, group, tooltip;
  ParamType type;
  ParamValue default_value;
  std::optional<double> min, max, step;     // numeric only; "soft" ranges flagged separately
  bool soft_range = false;                  // Unity [Range] is hard; plain fields may exceed
  std::vector<std::string> enum_values;     // Enum only, in C# declaration order
  bool advanced = false;                    // collapsed by default in the GUI
};
using ParamMap = std::map<std::string, ParamValue>;

struct Capabilities {
  bool progress = false;           // reports fractions
  bool cooperative_cancel = false; // polls ProgressSink::cancelled()
  bool deterministic = false;      // same input+params+threads -> identical output bytes
  bool thread_safe = false;        // may run concurrently in one process (worker never relies on it)
  int  max_threads = 1;            // 0 = backend decides
};

struct BackendInfo {
  std::string id;            // "fake.subdivide", "quadremesh.src", "quadremesh.dll", ...
  std::string display_name, version;
  Convention expected_convention;
  Capabilities caps;
  std::vector<ParamSpec> params;
};

class ProgressSink {             // implemented by the worker (JSON lines) and by tests
 public:
  virtual ~ProgressSink() = default;
  virtual void progress(std::string_view stage, std::optional<double> fraction) = 0;
  virtual void log(LogLevel, std::string_view msg) = 0;
  virtual bool cancelled() const noexcept = 0;   // cheap atomic load; poll at least every 100 ms
};

struct RemeshOutput { PolyMesh mesh; std::string backend_stats_json; };

class IRemeshBackend {
 public:
  virtual ~IRemeshBackend() = default;
  virtual const BackendInfo& info() const = 0;
  // Input is already converted to info().expected_convention and validated.
  // Must not throw: adapters catch everything and return Errc::BackendFailed / Internal.
  virtual Result<RemeshOutput> remesh(const PolyMesh& in, const ParamMap& params, ProgressSink&) = 0;
};

// Registry: backends register at static-init time via a macro in their own library,
// and the cli links them explicitly (no dlopen plugin system in v1).
const std::vector<IRemeshBackend*>& backends();
IRemeshBackend* find_backend(std::string_view id);

// Facade: the only entry point the cli calls.
//  1. validate input (policy from backend: e.g. require manifold)   -> InputInvalid
//  2. validate/complete params against ParamSpec (fill defaults, range-check) -> ParamInvalid
//  3. convert convention to backend's, call backend, convert back
//  4. validate output (indices, finite, optional manifold)          -> OutputInvalid
Result<RemeshOutput> remesh(std::string_view backend_id, const PolyMesh& in,
                            const ParamMap& params, ProgressSink& sink);
}
```

Planned adapters:

| Backend id | Branch | What it does |
|---|---|---|
| `fake.subdivide` | always | Splits every n-gon into n quads (face point plus edge midpoints, no smoothing), `iterations` 0..4. Debug knobs: `debug.sleep_ms_per_iter`, `debug.fail`, `debug.crash` (null write), `debug.hang` (ignores cancel), `debug.ignore_cancel`. Gives the protocol, GUI and parity harness a real end-to-end path before intake. |
| `quadremesh.src` | S | Calls the vendored source through whatever entry point the C# side uses (B: `docs/abi.md`). |
| `quadremesh.dll` | D | `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR \| LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` plus `GetProcAddress` per `docs/abi.md`. Struct layouts are pinned with `static_assert(sizeof/offsetof)` against the C# `StructLayout`. Windows x64 only. |
| `quadremesh.port` | P-b | C++ port of the C# algorithm. |
| *(external worker)* | P-a | A .NET 8 console worker that hosts the plugin's C# algorithm and speaks the same JSON-lines protocol. Not linked: the GUI launches a different executable. |
| `oss.quadriflow` / `oss.instantmeshes` | L | Open-source fallback. Top-level licences were checked as BSD-3-style; sub-dependencies were not. |

## 4. Process model and worker protocol (version 1)

The GUI runs **one worker process per remesh job**. Rationale and reversal criteria are in `docs/decisions.md` D2.

### Invocation

```
rdesk remesh --backend <id> --in <job>/input.rdm --out <job>/output.rdm
             [--params <job>/params.json] [--protocol jsonl] [--threads N]
rdesk backends --json            # BackendInfo[] incl. ParamSpec — the GUI builds its panel from this
rdesk info <mesh> [--json]       # stats + validation report
rdesk compare <ref> <test> [--json] [--tier T0|T1|T2]   # parity metrics (docs/testing.md)
rdesk convert <in> <out> [--in-convention unity|rh]     # RDM <-> OBJ
rdesk --version
```

Humans use the same binary with `--protocol text`, which is the default when stdout is a TTY. Meshes travel as
**files** in a job folder, never over pipes. Files are debuggable (the job folder is the bug report) and need no
framing. RDM write/read costs O(size): about 24 MB for 1M vertices with 1M quads (UNVERIFIED timing; measured in A9).

Job folder: `%LOCALAPPDATA%\RemesherDesk\jobs\<yyyymmdd-hhmmss>-<4 hex>\` on Windows,
`$XDG_STATE_HOME/remesherdesk/jobs/...` on Linux. The last 10 jobs are kept. It holds `input.rdm`, `params.json`,
`output.rdm`, `events.jsonl` (a copy of stdout), `stderr.txt`, `result.json`.

### stdout: events, one JSON object per line (UTF-8, `\n`, flushed per line)

Every event has `"v":1` (protocol version), `"t"` (type) and `"ms"` (milliseconds since worker start, monotonic).

| `t` | Fields | Notes |
|---|---|---|
| `hello` | `pid`, `rdesk` (version), `backend`, `backend_version`, `caps{progress,cooperative_cancel,deterministic}` | always first |
| `progress` | `stage` (string), `frac` (0..1 or `null` = indeterminate), `msg?` | `frac` is overall, never decreases (the worker clamps it), at most 20 per second |
| `log` | `level` (`debug`/`info`/`warn`/`error`), `msg`, `code?` | |
| `result` | `out` (path), `stats{in{v,f,tri,quad,ngon}, out{…}, ms{load,remesh,write,total}}`, `output_sha256` | exactly once on success |
| `error` | `code` (`E_USAGE` … `E_INTERNAL`, mirrors `Errc`), `msg`, `detail?` (object) | exactly once on failure |
| `bye` | `exit` (the exit code about to be returned) | always last, when the worker exits normally |

Example:

```
{"v":1,"t":"hello","ms":3,"pid":4242,"rdesk":"0.1.0","backend":"fake.subdivide","backend_version":"1","caps":{"progress":true,"cooperative_cancel":true,"deterministic":true}}
{"v":1,"t":"progress","ms":15,"stage":"load","frac":0.02}
{"v":1,"t":"progress","ms":410,"stage":"remesh","frac":0.61}
{"v":1,"t":"result","ms":980,"out":"C:\\...\\output.rdm","output_sha256":"…","stats":{…}}
{"v":1,"t":"bye","ms":981,"exit":0}
```

stderr carries free-form human logs. The GUI saves it to `stderr.txt` and never parses it.

### stdin: control, one JSON object per line

- `{"t":"cancel"}` sets the cancel flag.
- **EOF on stdin is treated as cancel.** If the parent died, the worker winds down. On POSIX this is the only
  orphan protection. On Windows the Job Object also covers it.

### Exit codes

| Code | Meaning | GUI state |
|---|---|---|
| 0 | success | show output |
| 2 | usage / bad arguments | internal bug: show the error and save a report |
| 3 | input read or parse error | "Could not read input" |
| 4 | input or parameters invalid | "Input not supported" (+ highlight) / "Invalid parameter" |
| 5 | backend failed, or output failed validation | "Remesh failed" + message |
| 6 | cancelled (cooperatively) | "Cancelled" |
| 7 | backend unavailable (DLL missing, wrong arch, init failed) | "Backend not available" + detail |
| 8 | output write error | "Could not write output" |
| 70 | internal error (uncaught exception) | "Internal error" + report |
| other / signal / NTSTATUS (e.g. `0xC0000005`) | crash | "Remesher crashed (code …)". The job folder is kept and can be saved as a bug report |

### Cancel semantics

1. The GUI writes `{"t":"cancel"}` and shows "Cancelling…".
2. Cooperative backends notice within ≤100 ms. The worker emits `error{code:E_CANCELLED}`, then `bye`, and exits 6.
3. If the process has not exited **1000 ms** after the cancel (configurable 250..10000), the GUI
   force-kills it: `TerminateJobObject` on Windows, `kill(SIGKILL)` on POSIX. The job is then shown as "Cancelled (forced)".
4. Requirement: the UI is back to idle within 1.25 s of the click in 20/20 trials, with both the cooperative
   and the `debug.hang` fake backend. Test: `tests/protocol/cancel_timing`.

The draft's "killed within 200 ms" is replaced. Killing first would skip native cleanup, so temp files
and partial output are possible, and the cooperative path is preferred whenever the backend supports it.

### Platform details (`src/platform`)

- **Windows.** Uses `CreateProcessW` with `CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT`, an explicit
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` so only the three pipe handles are inherited, and `AssignProcessToJobObject`
  with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, then `ResumeThread`. The worker calls
  `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)` and `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)`
  first thing. Without this, a crash can stall on a Windows Error Reporting dialog instead of exiting (UNVERIFIED
  on current Windows CI images; A9 has a test for it). Command lines are built with proper quoting
  (`CommandLineToArgvW` rules). All paths are UTF-16 at the Win32 boundary and UTF-8 everywhere else.
- **POSIX.** Uses `posix_spawn` with `O_CLOEXEC` pipes, `kill(SIGKILL)` and `waitpid`. `SIGPIPE` is ignored in both processes.

## 5. Threading

- **Worker.** The main thread runs load → facade → write. A stdin thread parses control lines into a
  `std::atomic<bool> cancel`. Event output goes through one mutex-guarded writer that flushes per line. The backend may use its own
  threads. `--threads` is passed through when the backend supports it. **(B)** Intake checks whether the native code uses
  OpenMP, TBB or `std::thread`; that affects determinism.
- **GUI.** The render/UI thread owns all state. `JobController` owns the `Process` and one reader thread that turns
  stdout lines into `Event` structs on a mutex-protected queue. The UI thread drains the queue each frame.
  States: `Idle → Starting → Running → Cancelling → {Succeeded | Failed | Cancelled | Crashed} → Idle`.
  Only one job runs at a time in v1. Loading meshes for display (OBJ parse, normals) runs on a `std::async` task with
  a "Loading…" overlay, so large files do not freeze the window.

## 6. Error handling and logging

- Modules return `Result<T>`. Exceptions are allowed inside a module but never cross a public API.
  Backend adapters wrap calls into third-party code in `try { } catch (...)`. Hardware faults are left to the process
  boundary: no SEH translation in v1.
- Worker: `main` catches anything left over, then emits `error{E_INTERNAL}` and `bye`, and exits 70.
- GUI log file: `%LOCALAPPDATA%\RemesherDesk\logs\remesherdesk-YYYYMMDD.log`, keeping 10 files, with plain-text lines
  `2026-11-04T12:00:00.123Z INFO [job] message`. A tiny in-house logger avoids a dependency.
  The log panel shows the same lines.
- "Save bug report" zips the job folder, the last log file and `rdesk --version` output to a path the user chooses.
  Nothing is uploaded anywhere.

## 7. Configuration and state

`%APPDATA%\RemesherDesk\settings.json` holds:

- recent files (10)
- last parameters per backend
- window geometry
- cancel grace time
- worker path override (for development)
- OBJ import convention (RH default, or "Unity LH" for OBJ files written by tools that do not convert)

Unknown keys are preserved. A corrupt file is renamed to `settings.json.bad` and defaults are used.
