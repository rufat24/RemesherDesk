# RemesherDesk

A standalone Windows desktop app for quad remeshing, built around the native remesher of the Unity plugin *Quadremesh* (no Unity runtime).

**Where things stand:** the whole pipeline around the remesher is built and tested (mesh I/O, validation, parity metrics, the `rdesk` worker with its JSON-lines protocol, process control, fuzzing, a GUI spike) using a placeholder `fake.subdivide` backend. The real remesher plugs in after the **intake** step (below), which only you can do. Read [docs/PLAN.md](docs/PLAN.md) first; its status table says what is done and where it was verified.

## Get the code

```bash
git clone https://github.com/rufat24/RemesherDesk
cd RemesherDesk
git checkout claude/amazing-dijkstra-eizqgr      # all work is on this branch; merge it into main when you are happy
```

(`claude/opus55-complete-plan` is an older copy of the same plan; ignore it.)

## Build and test on Windows

You need: Visual Studio 2022 or newer (or Build Tools) with the **Desktop development with C++** workload, CMake 3.25 or newer, Git, and Python 3. Use a "Developer PowerShell for VS". The first configure downloads dependencies with Git (doctest, nlohmann/json, tinyobjloader, and for the GUI Polyscope with its submodules), so it needs internet and takes a minute or two.

```powershell
# Everything except the GUI (fast):
cmake --preset msvc
cmake --build --preset msvc-release
ctest --preset msvc-release

# Everything including the GUI (what CI builds; warnings are errors):
cmake --preset ci-msvc
cmake --build --preset ci-msvc
ctest --preset ci-msvc
```

Binaries end up under `build\<preset>\...\Release\`: `rdesk.exe` (the worker and command-line tool) and, with `ci-msvc`, `src\app\Release\RemesherDesk.exe`.

```powershell
.\build\ci-msvc\src\cli\Release\rdesk.exe backends --json
.\build\ci-msvc\src\cli\Release\rdesk.exe info tests\data\mixed.obj
.\build\ci-msvc\src\cli\Release\rdesk.exe remesh --backend fake.subdivide --in tests\data\cube_unity.rdm --out out.rdm --protocol text
.\build\ci-msvc\src\app\Release\RemesherDesk.exe tests\data\mixed.obj      # the viewer spike; press W to toggle the wireframe
```

If a test fails on your machine, that is useful information: CI runs the same suite on `windows-latest`.

## Build and test on Linux

```bash
sudo apt-get install -y cmake ninja-build g++ clang libclang-rt-18-dev          # toolchain
cmake --preset ci-gcc   && cmake --build --preset ci-gcc   && ctest --preset ci-gcc
cmake --preset ci-clang && cmake --build --preset ci-clang && ctest --preset ci-clang   # + ASan/UBSan

# GUI and its screenshot test (needs X11/OpenGL headers and Xvfb):
sudo apt-get install -y xorg-dev libgl1-mesa-dev libgl1-mesa-dri xvfb
cmake --preset ci-gui && cmake --build --preset ci-gui && ctest --preset ci-gui -L gui

# Fuzzing (clang + libFuzzer):
cmake --preset clang-fuzz && cmake --build --preset clang-fuzz
build/clang-fuzz/tests/fuzz_obj -max_total_time=600 some_corpus_dir
```

`ctest -L unit|property|protocol|fuzz|gui|tools` runs one group. The protocol suite (`-L protocol`) takes about 20 s because it runs 20 cancel and 20 forced-kill trials.

## What to do next

1. **Intake (you, about 15 minutes; blocks the real remesher).** Make this GitHub repo **private** first. Then follow [docs/intake.md](docs/intake.md): run `tools\intake\Inventory.ps1` on the Unity project (it copies no source), send the resulting `qr_intake.zip`, and answer the three questions in [docs/risks.md](docs/risks.md) §2. The inventory decides which of four paths is taken (compiled source, prebuilt DLL, port of Burst/C# code, or an open-source fallback).
2. **Golden data.** Copy `tools/unity/MeshDump.cs` into the Unity project and dump 3 to 5 before/after meshes ([docs/intake.md](docs/intake.md) §2).
3. **Remaining work that needs no input:** A11 GUI shell, A12 headless self-test, A13 packaging; see [docs/work-breakdown.md](docs/work-breakdown.md).

## Layout

| Path | What |
|---|---|
| `src/core` | mesh model, validation, conversion, metrics, backend interface and facade (standard library only) |
| `src/io` | RDM and OBJ readers/writers, JSON codec |
| `src/platform` | process spawning/kill, UTF-8, paths (Windows and POSIX) |
| `src/backends/fake` | the placeholder test backend |
| `src/cli` | `rdesk`: the worker and command-line tool |
| `src/app` | the GUI (currently the Polyscope spike) |
| `tests/` | unit, property, protocol, fuzz (corpora and regression inputs), gui, data |
| `tools/` | intake script, Unity dump script, reference RDM reader |
| `docs/` | plan, architecture, decisions, testing, CI notes, GUI spec, licensing, risks |

## Working with Claude Code locally

Open the repo and start from `docs/PLAN.md` (status table and "How a fresh session should start" in §8). Each task in `docs/work-breakdown.md` lists its files and exit checks, so a new session needs no other context.
