# RemesherDesk — Plan

RemesherDesk turns the **native remesher** of the Unity plugin *Quadremesh* (the user's machine: `C:\Users\user\UnityProjects\Quadremesh`) into a standalone Windows desktop app. No Unity runtime is involved.

This document is the entry point and the single source of truth. Detail lives in the linked files.

| Document | Contents |
|---|---|
| [decisions.md](decisions.md) | Decision log: every choice, the alternatives, and what evidence would reverse it |
| [architecture.md](architecture.md) | Repo layout, modules, data model, core API, worker protocol, threading, errors, logging |
| [rdm-format.md](rdm-format.md) | The RDM binary mesh container (Unity dumps, worker IPC, fixtures) |
| [intake.md](intake.md) | Intake gate B0: inventory script, Unity dump script, decision tree, per-branch milestones |
| [work-breakdown.md](work-breakdown.md) | Every task (Part A now, Part B gated) with exit checks; the session schedule; the status table |
| [testing.md](testing.md) | Test layers, golden data, parity tiers and tolerances, determinism, property tests, fuzzing |
| [ci.md](ci.md) | CI matrix, cost control, local equivalents, observed runs |
| [gui.md](gui.md) | Screens, parameter panel, viewer, progress/cancel, error states, shortcuts, acceptance checklist |
| [packaging-licensing.md](packaging-licensing.md) | Licence audit, third-party matrix, Asset Store EULA, zip, signing, updates |
| [risks.md](risks.md) | Risk register, open questions with defaults, Definition of done for v1 |

> **What is known.** The Unity project cannot be reached from cloud sessions. Its language, algorithm, entry points, data layout and licence are **unknown**. This plan does not assume any of them. Everything that depends on them sits behind the intake gate (B0) and is marked **(B)**. Claims that could not be checked offline are marked **UNVERIFIED**. §7 lists what *was* checked in the planning session.

---

## 1. Executive summary

We build a small C++20 application in three parts:

- **`rdesk`** — a command-line tool that also serves as the **worker process**.
- **`RemesherDesk`** — a GUI (Polyscope/ImGui) that runs each remesh in a fresh `rdesk` worker. It talks to the worker over a versioned JSON-lines protocol and exchanges meshes through RDM files.
- **A core library** that knows nothing about Quadremesh. Remeshers plug in through an `IRemeshBackend` adapter.

There are four possible adapters. Which one we build depends on what intake finds:

- compiled-from-source (branch **S**)
- prebuilt DLL (branch **D**)
- hosted or ported C#/Burst (branch **P**)
- an open-source fallback if the licence forbids using Quadremesh (branch **L**)

About 10 sessions of work need no Quadremesh source: core, I/O, metrics, the worker, the GUI and packaging (Part A). Every branch reuses all of it. Intake costs the user about 15 minutes: run one PowerShell script and one Unity menu item, then answer three questions. It decides the branch before anyone touches the source. Parity with Unity is proven against **raw Unity mesh dumps**, using tiered tolerances that are justified in [testing.md](testing.md).

## 2. Goals, non-goals, success criteria

**Goals (v1)**

1. A Windows x64 desktop app that loads a mesh, runs the same remesh as Quadremesh does in Unity with the same parameters, shows before and after, and exports the result.
2. Results match Unity's within the agreed parity tier, proven on at least 3 golden cases.
3. Robust: a remesher crash or hang never takes down the GUI, and cancel always works.
4. Every session can rebuild and re-verify the whole project from the repo alone.

**Non-goals (v1)**

- macOS builds. Linux is used for development and CI only.
- FBX export, glTF export (glTF cannot store quads), USD.
- Mesh repair or editing. Invalid input is reported and highlighted, not fixed.
- UV, normal or skinning transfer onto the remeshed output, unless Quadremesh itself does it (decided at B5).
- Batch processing UI. The CLI `rdesk` covers scripting.
- Running inside Unity, or any dependency on the Unity runtime.
- Auto-update, telemetry, network access of any kind.

**Success criteria (measurable; full checklist in [risks.md](risks.md) §3)**

| # | Criterion | Target | Measured by |
|---|---|---|---|
| SC1 | Parity on the golden cases | branch tier: S → T1 Win / T2 Linux; D → T0 Win; P → T2; L → n/a | `tests/parity`, `docs/parity-report.md` |
| SC2 | Cancel responsiveness | UI idle ≤ 1.25 s after Cancel, 20 of 20 trials, including a non-cooperative backend | `tests/protocol/cancel_timing` + GUI checklist |
| SC3 | Crash isolation | backend crash → error banner, GUI alive, job folder saved | protocol test `debug.crash` |
| SC4 | Performance | desktop remesh time ≤ 1.2× Unity's on case03; worker overhead ≤ 300 ms | user measurement + A9 timing |
| SC5 | Scale | 1M-triangle OBJ loads ≤ 3 s; views at ≥ 30 fps | A4/A11 measurements |
| SC6 | Start-up | window visible ≤ 2 s cold | user measurement |
| SC7 | Robust I/O | fuzzing ≥ 1 h per reader with no open findings | `fuzz.yml` |
| SC8 | Distribution | zip ≤ 50 MB (excluding the backend) runs on a clean Windows runner (`--selftest`) | A13 CI job |

## 3. Decisions at a glance

Full reasoning, alternatives and reversal evidence are in [decisions.md](decisions.md).

| # | Decision | vs. draft |
|---|---|---|
| D1 | Single private repo `rufat24/RemesherDesk`; private **before** any Unity material is pushed | kept |
| D2 | One worker process per remesh; JSON-lines protocol v1; meshes via files; cooperative cancel, then kill after 1 s; Win32 Job Object; no WER dialog | kept, changed |
| D3 | Polyscope + ImGui + nfd-extended, gated by a 1-session spike; fallback is plain ImGui + our own GL 3.3 renderer | kept, conditional |
| D4 | Golden data = raw Unity arrays in the RDM container, plus boundary capture at the DllImport call (B) | kept, strengthened |
| D5 | OBJ + RDM in v1; PLY optional; glTF import later; no FBX | changed (PLY demoted) |
| D6 | CMake ≥ 3.25 + presets; FetchContent pinned by hash; no package manager | kept |
| D7 | C++20 + in-house `Result<T>` (`std::expected` unusable with clang 18 + libstdc++ 13, verified) | new |
| D8 | float32 CSR polygon mesh, per-vertex attributes, RH Y-up CCW internally | new |
| D9 | Unity ↔ RH: negate X + reverse winding keeping corner 0 (exact, tested) | made exact |
| D10 | doctest, nlohmann/json, tinyobjloader, own process layer | new |
| D11 | Ship Windows x64 only | kept |
| D12 | GitHub Actions; Windows jobs gated | new |
| D13 | Parity tiers T0/T1/T2 instead of one tolerance | new |
| D14 | Portable zip first; installer and signing later | new |

## 4. Architecture in one picture

Details are in [architecture.md](architecture.md).

```
 RemesherDesk.exe (GUI)                      rdesk.exe (worker / CLI)
 ┌──────────────────────────┐  spawn+Job   ┌────────────────────────────────────┐
 │ AppController            │ ───────────▶ │ load RDM/OBJ → facade.remesh()     │
 │ JobController ◀─ events ─┼── stdout ────┤   validate · params · convention    │
 │  (reader thread)  cancel ┼── stdin ────▶│   IRemeshBackend                    │
 │ Viewer (Polyscope/ImGui) │              │    ├ fake.subdivide (always)        │
 │ links: core, io, platform│◀─ files ────▶│    ├ quadremesh.src  (S)            │
 └──────────────────────────┘ job folder   │    ├ quadremesh.dll  (D, Windows)   │
                                           │    └ oss.*           (L)            │
                                           └────────────────────────────────────┘
        (branch P-a: a .NET 8 worker implements the same protocol)
```

## 5. Plan of work

The full task list, with files, steps and exit commands, is in [work-breakdown.md](work-breakdown.md).

**Part A — now, no source needed**

| ID | Task |
|---|---|
| A0 | Scaffold, plan and intake tools (**done**) |
| A1 | CI green on Linux and Windows |
| A2 | Core types and validation |
| A3 | RDM C++ I/O |
| A4 | OBJ I/O |
| A5 | Metrics and Hausdorff |
| A6 | Fuzzing |
| A7 | Backend API, facade, fake backend |
| A8 | Process layer |
| A9 | Worker and protocol conformance |
| A10 | GUI spike |
| A11 | GUI shell |
| A12 | GUI tests and `--selftest` |
| A13 | Packaging skeleton |
| A14 | *(optional)* Unity RDM importer |
| A15 | *(optional)* PLY |

**Part B — gated on intake.** B1 is the intake review. It chooses one branch:

| Branch | Task sequence |
|---|---|
| S | S1 build → S2 ABI + boundary capture + adapter → B3 parity → B4 progress/cancel → B5 parameters → B6 release |
| D | D1 ABI + loadability → D2 adapter → B3–B6 |
| P | P0 feasibility → P-a .NET worker (or P-b port) → B3–B6 |
| L | L1 OSS backend → L2 quality report → B6 |

**Schedule.**

1. Session 1 = A0 (this).
2. Sessions 2–10 = A1…A13. Some tasks run in parallel; see the table in work-breakdown §3.
3. **B1 jumps the queue** as soon as the inventory arrives.
4. A branch then takes about 3–8 sessions (UNVERIFIED estimates).

## 5b. Status (updated at the end of the A10 session)

| Task | State | Verified on |
|---|---|---|
| A0 scaffold, intake tools | done | Linux, Windows |
| A1 CI | done (7 workflow runs so far; every failure was found and fixed, see `docs/ci.md` §5) | GitHub Actions |
| A2 core types, validation, conversion | done | gcc, clang+ASan/UBSan, MSVC |
| A3 RDM reader/writer | done | gcc, clang+ASan/UBSan, MSVC |
| A4 OBJ import/export | done | gcc, clang+ASan/UBSan, MSVC |
| A5 metrics, distances, `compare` T0/T1/T2 | done | gcc, clang, MSVC (sample checksum identical on all three) |
| A6 fuzzing | done: 3 targets, replay tests, 10-minute runs; **2 real bugs found and fixed** | clang+libFuzzer; replay on all |
| A7 backend interface, facade, fake backend | done | gcc, clang, MSVC |
| A8 process layer | done on Linux; Windows `protocol.all` passed on a real runner | gcc, clang, TSan, MSVC |
| A9 `rdesk` worker CLI and protocol conformance | done | gcc, clang+ASan/UBSan, MSVC (`protocol.all` green on Windows) |
| A10 GUI spike | done on Linux; Windows build is in the latest CI run | gcc + xvfb; MSVC pending |
| A11 GUI shell, A12 selftest, A13 packaging | not started (no user input needed) | |
| **B1 intake review, then the branch (S/D/P/L)** | **blocked on the user** (see below) | |

## 6. What the user does next

1. Make the repo **private** (D1, R14).
2. Answer **Q1–Q3** ([risks.md](risks.md) §2).
3. Run `tools/intake/Inventory.ps1` and send `qr_intake.zip` ([intake.md](intake.md) §1).
4. Copy `tools/unity/MeshDump.cs` into the Unity project and dump 3–5 before/after cases ([intake.md](intake.md) §2). This can wait until after step 3.

## 7. Verification record of the planning session (A0)

**Checked in the container** (Ubuntu 24.04, CMake 3.28.3, GCC 13.3, Clang 18.1.3, Python 3, .NET SDK 8.0.133, PowerShell 7.6.9, mingw-w64):

- The CMake presets `ci-gcc` (Release, `-Werror`), `ci-clang` (Debug + ASan/UBSan, `-Werror`) and `gcc-debug` configure and build, and pass ctest: 2/2 tests each.
  - The ASan link needed `libclang-rt-18-dev`, which was installed.
- `<expected>` fails with clang 18 + libstdc++ 13 under C++20 and C++23. With gcc 13 it works only under C++23. This decided D7.
- `tools/rdm/rdm.py selftest` passes. It covers:
  - round trip
  - 8-byte alignment
  - a volume of 1.0 for the cube in both conventions
  - negative volume after a half-conversion
  - double conversion is the identity
  - unknown chunks are skipped
  - out-of-range indices and truncated files are rejected
- The C# 7.3 `RdmWriter` (from `tools/unity/MeshDump.cs`), compiled on .NET 8, wrote a cube that `rdm.py check-fixture` accepted. The Unity half of `MeshDump.cs` type-checks against hand-written Unity API stubs, with warnings treated as errors.
- `tools/intake/Inventory.ps1` passed `Test-Inventory.ps1` on PowerShell 7.6 (Linux) with a synthetic project. The project included a real x64 PE DLL with exports (built with mingw), a managed DLL, a non-ASCII path, `Temp/` and `com.unity.*` content to exclude, and literal and constant `DllImport` names.
- Two claims about the draft's PowerShell one-liner were checked in pwsh:
  - Its `extern static` regex misses `static extern` (the result is `False`).
  - Its `-in … -or …` precedence is correct.
- The top-level LICENSE files of 18 relevant repositories were read on their default branches ([packaging-licensing.md](packaging-licensing.md) §2).

**Not checked (UNVERIFIED):**

- **Polyscope build and API.** Building third-party code was blocked by the session's sandbox policy; the A10 spike covers this.
- **The MSVC preset and the Windows CI job** (A1).
- **Windows PowerShell 5.1 behaviour of the inventory.** CI runs it once A1 is done.
- **MeshDump.cs inside a real Unity Editor.**
- **The repository's current public or private visibility.**
- **GitHub Actions pricing.**
- **Asset Store EULA terms.**
- **Every timing target.**

## 8. How a fresh session should start

```bash
# 0. Orientation (read in this order)
cat docs/PLAN.md docs/work-breakdown.md            # then the docs the chosen task links to
git log --oneline | head -20                       # what happened recently
grep -n "Status" -A30 docs/work-breakdown.md | tail -30   # task status table

# 1. Toolchain (fresh cloud container; cmake/gcc/clang/ninja/python3 are preinstalled)
apt-get install -y libclang-rt-18-dev              # ASan/UBSan runtime for the ci-clang preset
# Only for intake-tool work:
#   curl -sSLo /tmp/pms.deb https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb \
#     && dpkg -i /tmp/pms.deb && apt-get update && apt-get install -y powershell dotnet-sdk-8.0 gcc-mingw-w64-x86-64

# 2. Build and test
cmake --preset ci-gcc   && cmake --build --preset ci-gcc   && ctest --preset ci-gcc
cmake --preset ci-clang && cmake --build --preset ci-clang && ctest --preset ci-clang

# 3. Pick the task
#   - If docs/intake-report.md does not exist and the user has sent qr_intake.zip → do B1.
#   - Else the first task in the status table that is "not started" and whose deps are done.
#     Right now that is A1 (+ A2 in the same session).
```

Rules for every session:

- Follow the task's exit checks.
- Never invent anything about Quadremesh; if a step needs it, the task belongs in Part B.
- Mark unverifiable claims UNVERIFIED.
- Update the status table and the docs you change.
- Commit with a descriptive message, and push to the branch the session was assigned.

## Provenance

- This plan replaces the earlier draft (`git show d736f54:docs/PLAN.md`). That draft also superseded the older `rufat24/RemesherDektop` `PLAN.md`.
- The draft's decisions are re-examined one by one in [decisions.md](decisions.md).
- The draft's PowerShell one-liner is replaced by `tools/intake/Inventory.ps1`. Its bugs are explained in [intake.md](intake.md) §1.
