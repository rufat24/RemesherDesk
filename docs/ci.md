# Continuous integration

The workflow lives in `.github/workflows/ci.yml`. A weekly fuzz workflow, `fuzz.yml`, arrives with task A6.

## 1. Matrix

| Job | Runner | What it runs | When |
|---|---|---|---|
| `linux-ci-gcc` | ubuntu-24.04 | preset `ci-gcc`: GCC Release, `-Werror`, all ctest | every push and PR |
| `linux-ci-clang` | ubuntu-24.04 | preset `ci-clang`: Clang Debug + ASan/UBSan, `-Werror`, all ctest | every push and PR |
| `intake-tools` | ubuntu-24.04 | `rdm.py selftest`; MeshDump.cs stub type-check; C# writer → Python reader agreement; Inventory.ps1 self-test with PowerShell 7 and a mingw-built DLL | every push and PR |
| `windows-msvc` | windows-latest | preset `ci-msvc`: MSVC x64 Release, `/W4 /WX`, all ctest; Inventory.ps1 self-test with **Windows PowerShell 5.1** | PRs, pushes to `main`, manual dispatch, and any push whose head commit message contains `[ci windows]` |
| `fuzz` (A6) | ubuntu-24.04 | libFuzzer targets for 10 min each | weekly cron; for PRs touching `src/io/**`, 60 s per target |
| `parity` (B3) | windows-latest + ubuntu | `tests/parity` on the golden cases | like `windows-msvc` |

The `ci-msvc` preset sets no generator on purpose. CMake then uses the newest Visual Studio it finds, so the job survives `windows-latest` image upgrades. Presets are verified locally only for `ci-gcc`, `ci-clang` and `gcc-debug`. The MSVC preset is UNVERIFIED until A1.

## 2. Cost control

- Public repositories: GitHub-hosted runner minutes are free (UNVERIFIED for the current plan terms; see GitHub's billing documentation).
- Private repositories (this repo after intake): minutes come out of the account's included quota. Windows minutes are billed at a higher rate than Linux (historically 2×, UNVERIFIED for current pricing). For that reason:
  - Windows runs only on PRs, `main`, manual dispatch, or a `[ci windows]` commit message.
  - `concurrency` with `cancel-in-progress` drops superseded runs on the same ref.
  - `timeout-minutes` is set on every job: 20 for Linux, 30 for Windows.
  - Artifacts are uploaded only from `main` and tags, with 7-day retention (A13).
  - ccache via `actions/cache` is added only when a job's build step goes over 5 minutes.
- If minutes become a real constraint, register the user's own Windows PC as a self-hosted runner. That costs no minutes, has a real GPU (so it can run GUI tests), and is the reference machine. The catch is that it is only available while the PC is on. Never let workflows from forks run on it.

## 3. Secrets and private material

- CI needs no secrets until signing (see `docs/packaging-licensing.md`).
- Vendored Quadremesh code and DLLs must never be uploaded as artifacts of a public repository. Their licence status is unknown until B1.

## 4. Local equivalents

| CI job | Local command |
|---|---|
| `linux-ci-gcc` | `cmake --preset ci-gcc && cmake --build --preset ci-gcc && ctest --preset ci-gcc` |
| `linux-ci-clang` | same with `ci-clang` (needs `libclang-rt-18-dev` for the ASan runtime; in a fresh cloud container run `apt-get install -y libclang-rt-18-dev` first) |
| `intake-tools` | the commands in `docs/testing.md` §7 (needs `dotnet-sdk-8.0`, `powershell`, `gcc-mingw-w64-x86-64`; see the `PLAN.md` "fresh session" section) |
| `windows-msvc` | in a VS 2022 Developer PowerShell: `cmake --preset ci-msvc; cmake --build --preset ci-msvc; ctest --preset ci-msvc` |

## 5. Observed runs

| Date | Commit | Result | Notes |
|---|---|---|---|
| 2026-11-04 | `0395699` (A0) | green: linux-ci-gcc, linux-ci-clang, intake-tools, windows-msvc (incl. Windows PowerShell 5.1 inventory test) | https://github.com/rufat24/RemesherDesk/actions/runs/41848848140 |
