# Decision log

Format for each decision: the **decision**, the alternatives considered, why this one was chosen, and the **evidence that would reverse it**.

The draft plan (`git show d736f54:docs/PLAN.md`) made decisions D2–D6. Each was re-examined here and is marked **KEPT**, **CHANGED** or **NEW**.

When a decision is reversed, edit its entry in place. Add a dated "Reversed:" line and leave the history in git.

---

### D1 — Work repository and privacy · KEPT

- **Decision:** `rufat24/RemesherDesk` is the only working repo; `RemesherDektop` is retired. The repo must be **private before any Unity material (source, DLLs, dumps, inventory) is pushed**.
- **Alternatives:**
  - A separate private "intake" repo, with RemesherDesk staying public. Rejected: cloud sessions would then need two repos, and the public repo would keep drifting towards needing private data anyway.
- **Status:** the draft said the repo is public. This session did not check its visibility (UNVERIFIED). It is the user's first action.
- **Reverse if:** the licence verdict (B0) is "fully open source, redistributable". Then the repo may go public, but only after the licence matrix has been filled in.

### D2 — One worker process per remesh · KEPT, with changes

- **Decision:** the GUI never runs remesh code in-process. Each job spawns the `rdesk` executable as a worker. Progress, logs, results and errors come back as JSON lines on stdout. Control messages go to stdin. Meshes are passed as RDM files.
- **Changes from the draft:**
  1. **Cancel:** cooperative cancel first, then a forced kill after a 1 s grace period. The draft killed straight away, which skips native cleanup.
  2. **Mesh transfer:** meshes go through files, not stdout.
  3. **Windows hardening:** a Windows Job Object (`KILL_ON_JOB_CLOSE`) and no WER dialog.
  4. **Protocol, not just a binary:** the protocol is specified independently of the C++ worker, so a .NET worker (branch P-a) can implement it too.
  5. **In-process facade for tests:** the facade is also callable in-process from tests and the CLI.
- **Alternatives:**
  - **In-process worker thread.** Rejected because the native code is unknown. It may have global or static state, may not be re-entrant, may crash, and may have no way to cancel. A crash would take the GUI and the user's unsaved view state with it.
  - **Persistent worker server.** One long-lived process handling many jobs. Rejected for v1: it adds lifecycle complexity for a saving of roughly tens of milliseconds of process start-up (UNVERIFIED; measured in A9).
  - **Shared memory for meshes.** Rejected: more complex than files, with little gain at the expected mesh sizes.
- **Reverse or modify if:**
  - Worker start-up plus backend initialisation is more than 10% of the median job time on the golden set. Then switch to a persistent worker.
  - RDM write plus read is more than 10% of job time. Then use shared memory.
  - The backend needs a GPU context shared with the viewer (for example compute shaders, see the intake hints). Then reconsider in-process execution behind a crash guard.

### D3 — GUI toolkit: Polyscope on Dear ImGui + GLFW · KEPT, conditional on a 1-session spike (A10)

- **Decision:** use Polyscope (MIT; the top-level `LICENSE` was checked on 2026-11-04) for the viewport and its ImGui context for panels. File dialogs come from `nativefiledialog-extended` (zlib; top-level `LICENSE` checked).
- **The spike must show all of the following, or the fallback is taken:**
  1. It builds with MSVC in CI and with gcc/clang on Linux.
  2. A polygon mesh with mixed triangles and quads renders without triangulation artefacts in the wireframe, under `xvfb-run` with Mesa (screenshot captured).
  3. The default Polyscope panels can be hidden and our own ImGui panels drawn (`polyscope::options::buildDefaultGuiPanels` / `state::userCallback`, UNVERIFIED API names to be confirmed against the pinned version).
  4. File drop and keyboard shortcuts are reachable.
  5. Release binary under 15 MB.

  Building Polyscope in the planning session was blocked by the sandbox policy, so none of the above was verified then.

- **A10 spike outcome (Polyscope v2.6.1, commit `0c3dd68`, MIT):**
  1. *Builds.* gcc 13 on Linux: yes (146 targets, about 4 minutes on 4 cores). It also configures with CMake 4.x. MSVC: see `docs/ci.md` §5 for the `windows-msvc` result.
  2. *Mixed polygons without triangulation artefacts: verified by eye.* `tests/data/mixed.obj` (22 triangles, 23 quads, 1 pentagon) under `xvfb-run` with Mesa llvmpipe (OpenGL 4.5 core): quads are drawn as quads with only their own edges, triangles show their one diagonal, the pentagon is a single polygon; colouring by corners-per-face (viridis 3 / 4 / 5) works. `tests/gui/screenshot_smoke.sh` checks that the image is non-blank (colour count, coverage, contrast) and runs as ctest `gui.screenshot_smoke` (1.6 s).
  3. *Own panels instead of the default ones: works, with two settings that the plan did not know about.* `polyscope::options::buildDefaultGuiPanels = false` hides the stock panels, and `polyscope::options::openImGuiWindowForUserCallback = false` removes the empty "user gui" window that Polyscope otherwise opens for `polyscope::state::userCallback`. Screenshots exclude the UI unless `ScreenshotOptions::includeUI = true`. Our panel's counts matched `rdesk info`.
  4. *File drop and shortcuts are reachable:* `polyscope::state::filesDroppedCallback` (GLFW drop) and `ImGui::IsKeyPressed` inside the callback are wired in the spike. Their interactive behaviour was not exercised here (no pointer or keyboard under Xvfb).
  5. *Size:* the Release executable is 10.7 MB (9.7 MB stripped), statically linked with Polyscope, ImGui and GLFW; target 15 MB.
- **Decision: Polyscope is kept; the fallback is not needed.** Also learned: Polyscope ships an **EGL** backend and a **mock** backend (`openGL3_egl`, `openGL_mock` were enabled by default), which may give A12's `--selftest --no-gl` a real headless path without Xvfb.
- **Not yet decided:** file dialogs. `nativefiledialog-extended` needs GTK3 (or xdg-desktop-portal) development packages on Linux; it is added in A11, where it is first used.
- **Fallback:** plain Dear ImGui + GLFW + a minimal OpenGL 3.3 renderer of our own (flat-shaded polygons, wireframe overlay, per-face colours, orbit camera). That is about one extra session. Panel code carries over unchanged because it is the same ImGui.
- **Alternatives:**
  - **Qt 6** (LGPL, dynamic linking): best native look and docking, but a heavy toolchain in CI, about 40 MB extra to deploy (UNVERIFIED), and the viewport would still need a custom renderer.
  - **Tauri or Electron with three.js:** the web UI could be tested with Playwright in the cloud, which is a real plus. But it adds a second language and toolchain, mesh streaming into a webview, and either Rust or about 150 MB of Chromium (UNVERIFIED). The worker protocol keeps this option open: a web shell could drive the same `rdesk` worker later.
  - **WinUI / WPF:** Windows-only, and not testable in Linux CI.
- **Why Polyscope:** a remesher UI mostly needs to *look at polygon meshes*:
  - quads drawn as quads
  - wireframe
  - colouring by face type and by vertex valence (singularities)
  - side-by-side comparison

  Polyscope does all of this out of the box. The fallback uses the same stack, so the risk is bounded.
- **Reverse if:** any spike criterion fails, the user's target machines lack OpenGL 3.3 (Remote Desktop, some VMs), or the UI needs native docking or multi-window. In those cases use the ImGui fallback, or Qt for the docking need.

### D4 — Golden data: raw Unity Mesh arrays in a versioned container · KEPT, strengthened

- **Decision:** golden data is captured by `tools/unity/MeshDump.cs` as **raw `Mesh` arrays** in the RDM container (`docs/rdm-format.md`):
  - no axis or winding conversion
  - indices widened to 32-bit
  - every submesh
  - the object transform
  - EditorJsonUtility dumps of the object's other components, which capture the remesher's inspector parameters if the remesher is a component

  The same container is used for worker IPC and test fixtures.
- **Strengthened beyond the draft:**
  1. The C# writer is checked against the Python reference reader in CI.
  2. A SHA-256 sidecar records what was sent.
  3. At B-stage (branches S/D), a **boundary capture** is added: a temporary C# shim around the `[DllImport]` call dumps exactly the arrays and parameters passed into native code and the arrays returned. Native parity is then tested apart from the C# pre- and post-processing.
- **Alternatives:**
  - **Unity-exported OBJ/FBX.** Rejected: exporters apply their own axis, winding and welding changes, and OBJ text loses float bits unless written with 9 significant digits.
  - **Unity's own `.asset` YAML.** Rejected: hard to parse, and the vertex buffer layout varies.
- **Reverse if:** the user cannot run Editor scripts (for example a locked-down Unity). Then fall back to OBJ exported with a known exporter and fixed settings, and lower the parity expectation to tier T2.

### D5 — File formats · CHANGED

- **Decision:**
  - **v1:** OBJ import and export, plus RDM import and export.
  - **Later:** PLY (binary) when large meshes need it. glTF **import only** (P3).
  - **Not planned:** FBX.
- **Changes from the draft:** PLY is demoted from "first" to optional. It adds little for a Unity user, whose interchange formats are FBX and OBJ, and each format adds fuzzing surface.
- **Why OBJ:** the only ubiquitous text format with native **n-gon faces**, so quads survive. glTF 2.0 primitives are points, lines and triangles only (modes POINTS…TRIANGLE_FAN), so quad output cannot be stored in glTF without triangulating. FBX needs either the proprietary Autodesk SDK or a third-party parser. `ufbx` (MIT, UNVERIFIED) could be a later import-only option.
- **OBJ details:**
  - Reader: `tinyobjloader` (MIT; top-level `LICENSE` checked) with `triangulate=false`, fuzzed by us.
  - **A4 implementation notes.** (1) The writer uses `std::to_chars` (shortest text that reads back to the same bits, locale-independent) instead of `%.9g`; a test round-trips 100,000 random float bit patterns plus signed zeros, denormals and extremes bit-exactly through tinyobjloader. (2) tinyobjloader loads the supplied `.mtl` text only when the OBJ contains an `mtllib` line, and without a `.mtl` every `usemtl` gets id -1; the reader therefore synthesises a minimal `.mtl` from the `usemtl` names and prepends an `mtllib` line to its private copy of the text, so that different materials become different `face_group` ids. (3) tinyobjloader is compiled in its own target without the project's warning flags (`rdesk_tinyobj`).
  - Writer: our own, with `%.9g`. Nine significant digits round-trip every float32 exactly.
  - Import convention: right-handed by default. A user setting chooses "Unity LH" for OBJ files from exporters that do not convert.
- **Reverse if:** the user's actual workflow is FBX in and out. Then add ufbx import (P2), and offer export to FBX only if a licence-clean writer exists.

### D6 — Build system: CMake with presets, pinned FetchContent · KEPT

- **Decision:**
  - CMake 3.25 or newer with `CMakePresets.json`. Verified in this container: CMake 3.28.3, gcc 13.3, clang 18.1.3; presets `ci-gcc`, `ci-clang` and `gcc-debug` configure, build and pass ctest.
  - Dependencies come through `FetchContent`, pinned by URL and SHA-256 or by an exact commit. There are expected to be 6 or fewer (doctest, nlohmann/json, tinyobjloader, polyscope, nfd-extended, and glfw/imgui via polyscope).
  - No vcpkg or conan.
- **Alternatives:**
  - **Meson:** good, but the vendored native code most likely ships with CMake or `.vcxproj` (UNVERIFIED).
  - **Bazel:** too heavy.
  - **MSBuild-only:** no Linux CI.
  - **vcpkg manifest mode:** worth it only with many binary dependencies.
- **Reverse if:**
  - More than 8 dependencies, or Windows CI build time over 10 min. Then switch to vcpkg with binary caching.
  - The vendored source needs a complex MSBuild-only build. Then wrap it with `include_external_msproject` or `ExternalProject` on Windows. Branch S still keeps CMake as the top level.

### D7 — Language level C++20 and an in-house `Result<T>` · NEW

- **Decision:** C++20 for our code. The vendored remesher is compiled with whatever standard and flags its own build uses (parity). `std::expected` is **not** used.
- **Evidence:** in this container `<expected>` failed to compile with clang 18 + libstdc++ 13 under both `-std=c++20` and `-std=c++23`. With gcc 13 it worked only under `-std=c++23`. A tiny `Result<T>` (about 80 lines) avoids depending on the toolchain.
- **Reverse if:** the CI images ship clang 19 or newer with libstdc++ 14 or newer everywhere. Then switch to `std::expected` with `-std=c++23`.

### D8 — Boundary data model: float32 CSR polygon mesh, per-vertex attributes, RH canonical · NEW

See `docs/architecture.md` §2.

- **Alternatives:**
  - **Double precision:** rejected because parity would suffer.
  - **Half-edge structure (OpenMesh, pmp) as the boundary type:** rejected; backends build their own internal structures.
  - **Per-corner attributes:** rejected because Unity is per-vertex.
- **Reverse if:** two or more backends need the same half-edge build. Then add a shared utility, and still do not change the boundary type.

### D9 — Unity ↔ RH conversion: negate X and reverse winding, keeping corner 0 · NEW (the draft named it, this makes it exact)

- **Decision:** as in `docs/rdm-format.md`. Verified by `rdm.py selftest`:
  - volume stays positive under a full conversion
  - a half-conversion gives negative volume
  - a double conversion gives back the original bits
- **Alternative:** negate Z, which is also a valid mirror. X was chosen because it is what OBJ exporters for Unity commonly do (UNVERIFIED).
- **Reverse if:** at intake, the user's DCC tool (for example Blender importing the same asset) shows the desktop output mirrored. It is one function and one setting.

### D10 — Third-party picks for v1 · NEW

| Need | Pick | Licence (top-level `LICENSE` read 2026-11-04 on the default branch; sub-components not checked) |
|---|---|---|
| unit tests | doctest | MIT |
| JSON | nlohmann/json | MIT |
| OBJ reader | tinyobjloader | MIT |
| viewer / UI | Polyscope (+ bundled Dear ImGui MIT, GLFW zlib) | MIT |
| file dialogs | nativefiledialog-extended | zlib |
| process spawning | in-house (`src/platform`) | — |

Process spawning is in-house because Job Objects, handle-list inheritance and the cancel semantics are specific enough that a library (reproc, Boost.Process) saves little. About 300 lines, all of it tested.

### D11 — Platforms · KEPT

Windows 10/11 x64 is the **only shipped target**. Linux (gcc and clang) is for development and CI only. macOS is a non-goal.

**Reverse if:** the user answers "I also need macOS/Linux builds" (open question Q3).

### D12 — CI on GitHub Actions with Windows gated · NEW

See `docs/ci.md`.

**Reverse if:** Actions minutes become a real cost. Then use a self-hosted Windows runner on the user's PC, which also enables real-GPU GUI tests.

### D13 — Parity in tiers rather than one tolerance · NEW (replaces the draft's single 1e-6/1e-4 numbers)

See `docs/testing.md` §3.

**Reverse if:** intake shows the remesher is purely continuous, with no discrete decisions. Then T2 topology equality may be required as well.

### D14 — Distribution: portable zip first · NEW

See `docs/packaging-licensing.md`.

**Reverse if:** the user wants Start-menu integration or file associations (then use an Inno Setup installer), or plans public distribution (then add code signing).

## A7 implementation notes (backend interface, facade, fake backend)

- **Explicit backend registration, not static initialisers.** `docs/architecture.md` §3 had backends registering themselves at static-init time. Static initialisers inside a static library are dropped by the linker unless something references the object file, so a backend could silently vanish from the CLI. Each backend library instead exposes a `register_*()` function that the executable calls; the registry is a function-local static behind a mutex. Reversal evidence: if backends ever become shared libraries loaded by name, self-registration is natural again.
- **Params JSON lives in `rdesk_io`** (`json_codec.hpp`), not in core. The task text put nlohmann/json next to `params.hpp`, which would have broken the rule that core depends on the standard library only.
- **`registered_backends()`** replaces the draft's `backends()`: that name collides with the namespace `rdesk::backends` that holds the backend libraries.
- **`InputRequirements`** (`require_manifold`, `require_closed`, `triangles_only`) is the "policy from backend" that §3 mentions but did not define.
- **Facade cancel rule.** A cancel that is set when the backend returns is reported as `Cancelled` even if the backend finished (a deaf backend such as `debug.ignore_cancel`), so the worker never reports success after the user pressed Cancel.

## A8 implementation notes (platform process layer)

- **Shared threaded layer, thin native backends.** `process.cpp` holds everything platform-independent (one reader thread per pipe, the line queue, waiting); `process_posix.cpp` / `process_win32.cpp` only spawn, read, write, kill and poll. Blocking reads in threads are simpler and more portable than overlapped I/O or `poll`, and the cost is two idle threads per job.
- **Backpressure, not loss.** The first version dropped the oldest line beyond 100,000 queued lines; a test showed that this could silently discard a protocol event. The reader now stops reading when the queue is full, so the child blocks in its own write. Reversal evidence: a caller that never reads stdout would stall a chatty child; the GUI always drains, and the worker emits at most ~20 events per second.
- **POSIX.** `posix_spawn` with its own process group (kill takes down grandchildren), `SIGPIPE` ignored in the parent but reset to default in the child (an ignored disposition would otherwise be inherited across exec), close-on-exec pipes plus `addclosefrom_np(3)` on glibc >= 2.34 so that no other descriptor leaks. Elsewhere the leak test is skipped, not faked.
- **Windows.** `CREATE_SUSPENDED` + explicit handle list + Job Object with `KILL_ON_JOB_CLOSE` assigned before `ResumeThread`, as specified. Exit codes with both top bits set are classed as crashes. Exit-code classification and the Job Object paths have only been exercised through the `windows-msvc` CI job.
- **Job folder names use UTC** (`yyyymmdd-hhmmss-xxxx`): local time would make the sort order wrong across a DST change.
