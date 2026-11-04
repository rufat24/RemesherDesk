# Risks, open questions, Definition of done

## 1. Risk register

Likelihood (L) and Impact (I) are rated H/M/L.

| # | Risk | L | I | Mitigation | Early-warning signal |
|---|---|---|---|---|---|
| R1 | **Licence forbids use outside Unity or redistribution** | M | H | Audit at B1, before any porting effort. Branch L keeps all of the A work useful. Ask the publisher for permission early. | Inventory `meta/` has an Asset Store EULA reference or no LICENSE at all; Q1 answer is "Asset Store" |
| R2 | **The native part is not native**: Burst/IL2CPP/C# only | M | H | Inventory detects it. Branch P-a (host C# in .NET) is cheaper than a port. | `summary.txt`: 0 native sources, Burst > 0 |
| R3 | **The prebuilt DLL depends on Unity** (UnityPlayer.dll, IUnityInterfaces, graphics device) | M | H | D1 loadability test on Windows CI before any adapter work. If it fails, re-branch to P or L. | `exports.txt` has `UnityPluginLoad`; dumpbin dependents list UnityPlayer |
| R4 | **C# pre/post-processing hides real logic** (welding, splitting, transform, cleanup) that parity depends on | H | M | Boundary capture (S2/D1) dumps native-call arguments. The adapter replicates the C# steps, each documented in `docs/abi.md`. | `after` from Unity is not explained by the native output alone |
| R5 | **Non-determinism** (threads, RNG, unordered containers, Burst float modes) makes parity unprovable | M | M | Tiered parity (`docs/testing.md` §3). Measure Unity's own run-to-run variation (`after` vs `after2`). `--threads 1` for T0. | `after` ≠ `after2`; our 5-run hashes differ |
| R6 | **GUI stack fails** (Polyscope API limits, OpenGL 3.3 unavailable on the user's machine or over RDP) | L–M | M | A10 spike with an exit criterion. ImGui fallback on the same stack. The CLI always works. | Spike criterion fails; user reports a GL error |
| R7 | **Algorithm partly on the GPU** (compute shaders) | L | H | Inventory flags `.compute` files used from C#. Then the worker needs a D3D11/GL compute context; re-open D2 and estimate a port of the shaders. | `summary.txt` ComputeShader > 0 |
| R8 | **Mesh welding/index semantics mismatch** (Unity UInt16, seam-split vertices, submeshes) | H | M | RDM keeps raw arrays. Explicit `weld_by_position`. Parity compares in Unity convention. | Vertex counts differ by roughly the seam count |
| R9 | **Coordinate convention error** (mirrored output, inverted normals) | M | M | Exact conversions with tests (volume sign, chirality fixture). Backface tint in the viewer. | Output looks inside-out or mirrored next to Unity |
| R10 | **Windows CI minutes cost** (private repo) | M | L | Gated Windows jobs, timeouts, concurrency cancel. Self-hosted runner option. | Billing page; job durations in `docs/ci.md` |
| R11 | **Cloud sessions lack context** (no memory between sessions) | H | M | Tasks are self-contained with exit commands. This plan is the single source of truth; each task updates the status table. | A session asks questions the docs answer → improve the docs |
| R12 | **Large meshes** (≥ 1M faces): memory or time in the worker or viewer | M | M | Measure in A4, A9 and A11. RDM is streaming-friendly. The viewer can decimate for display (later). | Load > 3 s or < 30 fps on 1M-tri input |
| R13 | **Remesher crash or hang leaves orphan processes** or WER dialogs on Windows | M | M | Job Object `KILL_ON_JOB_CLOSE`, `SetErrorMode`, stdin-EOF cancel; tested in A8/A9. | Protocol test timeouts on Windows |
| R14 | **Repo still public when Unity material is pushed** | M | H | Step 0 of intake. The intake session checks visibility with the GitHub tools before asking for the push. | — |

## 2. Open questions for the user

Each question has a recommended default. If a question is unanswered, the default applies.

| # | Question | Recommended default |
|---|---|---|
| **Q1** | **Where does Quadremesh come from?** Your own code, the Asset Store (which asset and publisher), or open source (which repository)? | Treat it as restricted: private repo, nothing distributed, until B1 clears it. |
| **Q2** | **Is the app for you only, or will you give or sell it to others?** | You only, on Windows x64. This keeps an AMBER licence verdict workable. |
| **Q3** | **How exact must results be?** Must they be identical to Unity's output, or is "same quality, may differ slightly" enough? | Tier T1 on Windows (T0 if it uses the same DLL), T2 on Linux (`docs/testing.md` §3). |
| Q4 | Which formats do you really move meshes in? | OBJ + RDM in v1; FBX import later if needed. |
| Q5 | Typical and maximum mesh size (triangles) and acceptable remesh time? | Typical 100k, maximum 1M; desktop time ≤ 1.2× Unity's. |
| Q6 | Do you need macOS or Linux builds? | No. |
| Q7 | Does Quadremesh run in the Editor only, at runtime, or both, and do you use it from scripts or only through the inspector? | Inspector, in the Editor. |
| Q8 | May the repo's default branch be renamed to `main` once the plan is merged? | Yes. CI's Windows gating assumes `main`. |
| Q9 | Can you leave your PC on as a self-hosted CI runner if minutes become an issue? | Not needed for now. |

## 3. Definition of done for v1

v1 is done when **all** of the following hold. B6 checks every item and records the evidence (CI run URLs, the parity report, screenshots) in `docs/release-v1.md`.

**Function**
1. On a clean Windows 10 or 11 x64 machine, the user unzips `RemesherDesk-1.0.0-win64.zip` and starts `RemesherDesk.exe` without installing anything. The window appears in ≤ 2 s.
2. The app opens OBJ and RDM files up to 1M triangles, runs the Quadremesh backend (or, for branch L, the OSS backend) with every inspector parameter available, and exports OBJ and RDM.
3. Progress is shown. Cancel returns the UI to idle in ≤ 1.25 s in 20 of 20 trials. A backend crash shows an error and never closes the GUI.

**Quality and parity**

4. Every golden case (at least 3) meets the branch's parity tier on Windows, and T2 on Linux where applicable. Measured values are in `docs/parity-report.md`.
5. Desktop remesh time is ≤ 1.2× Unity's on the same machine for case03, as measured by the user.

**Engineering**

6. All CI jobs are green on the release tag: Linux gcc, Linux clang+ASan/UBSan, intake tools, Windows MSVC, parity. Fuzzing has run for ≥ 1 h per target since the last change to `src/io`, with no findings open.
7. The GUI acceptance checklist (`docs/gui.md` §8) passes with the real backend.
8. `docs/` matches the code. The status table in `docs/work-breakdown.md` is complete. `CHANGELOG.md` has a 1.0.0 entry.

**Licensing**

9. `docs/licenses.md` has a verdict (GREEN, or AMBER accepted by the user). `THIRD_PARTY_NOTICES.txt` and `licenses/` ship in the zip. The distribution matches the verdict (§3 of `docs/packaging-licensing.md`).
