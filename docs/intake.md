# Intake gate B0

Nothing about Quadremesh is known yet: its language, algorithm, entry points, data layout and licence. This gate turns that unknown into a recorded decision. **No work in Part B may start until `docs/intake-report.md` exists and names a branch** (S, D, P or L).

## 0. Order of operations

| Step | Who | What | Output |
|---|---|---|---|
| 0 | user | Make `rufat24/RemesherDesk` **private** (GitHub → Settings → General → Danger Zone → Change visibility). | private repo |
| 1 | user | Answer questions Q1–Q3 in `docs/risks.md` §2 (origin and licence, personal or distributed, parity expectation). | answers in chat or an issue |
| 2 | user | Run the inventory (§1). Review the output folder, then send `qr_intake.zip` (chat upload is fine: it contains no source files, only file names, hashes, single interop lines and licence/metadata files). | `qr_intake.zip` |
| 3 | session | Analyse the inventory. Write `docs/intake-report.md` (template in §5) and the first `docs/licenses.md`. Pick a branch with the decision tree (§4). | branch decision |
| 4 | user | If the licence verdict allows: push the source (§3) and golden dumps (§2). | `intake` branch |
| 5 | session | Start the branch's first milestone. | — |

Steps 2 and 3 are cheap, and they happen before any source leaves the machine.

## 1. Inventory: `tools/intake/Inventory.ps1`

It is read-only and writes only to the output folder. It needs no Visual Studio: PE headers and export tables are parsed inside the script. If `dumpbin` is on PATH (Developer PowerShell for VS), its `/dependents /exports` output is added.

```powershell
# From a normal PowerShell window, in the folder where you saved Inventory.ps1:
powershell -NoProfile -ExecutionPolicy Bypass -File .\Inventory.ps1
# or with explicit paths:
powershell -NoProfile -ExecutionPolicy Bypass -File .\Inventory.ps1 -ProjectPath 'C:\Users\user\UnityProjects\Quadremesh' -OutDir "$env:USERPROFILE\Desktop\qr_intake"
```

`-ExecutionPolicy Bypass` applies to this one process only. It does not change machine policy.

### What it collects

It scans every top-level folder except `Library`, `Temp`, `Logs`, `obj`, `UserSettings`, `Build(s)`, `.git`, `.vs` and `.idea`. It adds back `Library\PackageCache`, where registry and embedded packages (possibly Quadremesh itself) live. Unity's own `com.unity.*` packages are excluded from everything except the binary hashes.

| File | Content |
|---|---|
| `summary.txt` | Counts and hints: native source, build files, GPU shaders, native vs managed DLLs with architecture and export counts, C# interop counts (`DllImport`, `extern`, Burst, Jobs, Marshal, ComputeShader), DllImport library names, whether `Library\BurstCache` exists. |
| `files.csv` | Relative path, category, size, and SHA-256 for binaries. |
| `binaries.csv` | Format (PE32/PE32+/ELF/Mach-O), machine, native or managed, export count. |
| `exports.txt` | Export names of native DLLs. |
| `interop_cs.csv` | **Every** matching C# line, as `file, line, kind, text` (each up to 300 chars). |
| `meta\` | Copies of `ProjectVersion.txt`, `manifest.json`, `packages-lock.json`, and every LICENSE/COPYING/NOTICE/README/THIRD-PARTY/CHANGELOG, `package.json` and `.asmdef` outside `com.unity.*` (each under 1 MB, at most 300). |
| `errors.txt` | Paths that could not be read. |

### What was verified, and how

- **Verified:** `tools/intake/tests/Test-Inventory.ps1` builds a synthetic Unity-like project, runs the inventory and checks its outputs. It ran with PowerShell 7.6 on Linux in the planning session and passed. The fixture includes:
  - a real x64 PE DLL with 3 exports (built with mingw-w64)
  - a managed .NET DLL
  - a `Native~` folder with C++ source and `CMakeLists.txt`
  - C# with `static extern` and `DllImport` (both literal and constant library names)
  - Burst/Jobs usage
  - a non-ASCII path (`Ünicode ə`)
  - `Temp/` and `com.unity.*` content that must be excluded

  CI runs the same test with PowerShell 7 on Linux (`intake-tools` job) and with **Windows PowerShell 5.1** on `windows-latest` (`windows` job).
- **UNVERIFIED** until that Windows job has run green: behaviour under Windows PowerShell 5.1, long paths (over 260 chars), OneDrive placeholder files.

### Bugs and pitfalls in the draft's one-liner, and how the script avoids them

1. **`extern static` pattern.** C# almost always says `static extern` (`private static extern int f(...)`). The draft's regex `DllImport|extern static|BurstCompile` missed those lines.
   - Verified: `'private static extern int f();' -match 'DllImport|extern static|BurstCompile'` returns `False` in pwsh 7.6.
   - The script matches `\bextern\b`, plus `[DllImport` and `[LibraryImport` separately.
2. **`Select-String -List`** keeps only the **first** match per file, so every DllImport after the first was lost. The script reads each file and records every matching line.
3. **Encoding.** In Windows PowerShell 5.1, `Export-Csv` defaults to ASCII, so non-ASCII path characters become `?`. `Out-File` defaults to UTF-16LE, which is awkward to grep on Linux. The script writes everything through `[IO.File]::WriteAllText(..., UTF8Encoding($false))` (UTF-8 without BOM).
4. **dumpbin dependency and noise.** The draft only listed exports when `dumpbin` was on PATH, and then ran it on every `.dll`, including dozens of managed assemblies. The script parses PE headers itself. It tells native from managed by the CLR data directory (entry 14), reads the machine type, and walks the export name table through the section table. dumpbin stays optional.
5. **Operator precedence.** The draft's `$_.Extension -in '.c',...,'.sln' -or $_.Name -match '...'` is actually **correct**: in PowerShell the comma (array) operator binds tighter than comparison operators, which bind tighter than `-or`, so it parses as `(Ext -in (list)) -or (Name -match ...)`.
   - Verified in pwsh 7.6: `'.x' -in '.c','.h' -or 'LICENSE' -match '^(LICENSE)'` is `True`, and `... -or 'foo' -match ...` is `False`.
   - The script still uses hashtable lookups, which are easier to read and to extend.
6. **Missed file kinds:**
   - `.mm .m .cu .ispc .asm`
   - `.compute .hlsl .cginc`: compute shaders, which may carry part of the algorithm
   - `.props .targets`
   - macOS `*.bundle` folders, which `-File` skipped
   - `package.json` and `.asmdef`, which identify the asset and its Burst dependency
7. **Silent errors.** `-ea 0` hid unreadable paths. The script collects them with `-ErrorVariable` into `errors.txt`.
8. **Hidden items.** `Get-ChildItem` without `-Force` skips hidden and system files. The script uses `-Force` and explicitly excludes `.git`, `.vs` and `.idea`.
9. **Alias shadowing (found while writing the test).** A helper function named `R` is silently shadowed by the built-in alias `r` → `Invoke-History`, because aliases take precedence over functions. Avoid one- or two-letter function names in scripts the user runs.
10. **Execution policy.** A `.ps1` file needs `-ExecutionPolicy Bypass` on default Windows configurations. A pasted one-liner does not.

## 2. Golden data: `tools/unity/MeshDump.cs`

Install it by copying `tools/unity/MeshDump.cs` to `Assets/RemesherDeskTools/Editor/MeshDump.cs`. Delete that folder when you are done. The script is read-only towards the project.

Procedure. Use at least 3 cases; 5 is better.

| Case | Mesh | Why |
|---|---|---|
| case01 | small closed mesh, ≤ 5k triangles (for example a Unity primitive or a simple prop) | fast tests, debuggable |
| case02 | open mesh with boundary loops and UV seams | boundary handling, welding |
| case03 | about 100k triangles, typical of your real use | performance and realism |
| case04 (opt.) | mesh with several submeshes | submesh/material handling |
| case05 (opt.) | a mesh where Quadremesh gives a visibly imperfect result | regressions are often near failure modes |

For each case:

1. In the Hierarchy, select the **input** object.
2. Run *Tools ▸ RemesherDesk ▸ Dump Selected Mesh(es)*.
3. Choose or create the folder `qr_dumps/caseNN_before`.
4. Run Quadremesh exactly as you normally do. **Write down** every parameter you changed and the time it took.
5. Select the **output** object (or the same object, if Quadremesh replaces the mesh in place).
6. Dump it into `qr_dumps/caseNN_after`.
7. Repeat the remesh once more on a fresh copy and dump it into `caseNN_after2`. This shows whether Unity's own result is deterministic, and it decides tier T0 (`docs/testing.md`).
8. Each `.rdm` file has a `.rdm.json` sidecar that is human-readable. Check it before sending.

Verified in this session:

- The pure-C# writer half (`RdmWriter`) compiled as C# 7.3 on .NET 8 and wrote a cube that the Python reference reader accepted bit-for-bit (`rdm.py check-fixture`).
- The Unity-dependent half type-checks against hand-written stubs (`tools/unity/tests/StubCheck`). That only proves the syntax and our assumptions about the API, not compatibility with real Unity.

**UNVERIFIED** until it compiles inside your Editor:

- the Unity API calls themselves (`GetIndices(int,bool)`, `GetVertexAttributeDimension`, `EditorJsonUtility.ToJson`, `SaveFolderPanel`)
- behaviour with meshes that do not have Read/Write enabled. In the Editor this usually works; the script warns if not.

The intake session fixes any compile errors you report.

## 3. Delivering the source (after the licence verdict allows it)

Push it to the **private** repo as an untouched first commit, so that any later edits show as diffs:

```powershell
cd C:\src
git clone https://github.com/rufat24/RemesherDesk
cd RemesherDesk
git switch -c intake
# Copy ONLY the folders the inventory identified (native source + build files + DllImport C# files + inspector/editor scripts):
robocopy "C:\Users\user\UnityProjects\Quadremesh\<native dir>" third_party\quadremesh\native /E
robocopy "C:\Users\user\UnityProjects\Quadremesh\<C# dir>" third_party\quadremesh\unity_cs /E /XF *.meta
robocopy "<qr_dumps folder>" tests\golden /E
git add -A
git commit -m "Vendor Quadremesh native + interop sources (unmodified) and golden dumps"
git push -u origin intake
```

- **robocopy exit codes 0–7 mean success.** PowerShell does not treat them as errors.
- **Do not use Git LFS.** Cloud sessions may not receive LFS objects (UNVERIFIED for this environment). Dumps for meshes of 100k or more triangles are a few MB each, which is fine in plain git.
- **Branch D:** also commit the DLL(s) under `third_party/quadremesh/bin/x86_64/`. Only do this in the private repo, and only if the licence permits copying it there.

## 4. Decision tree

```
Inventory says…                                    → Branch
──────────────────────────────────────────────────────────────────────────────────────────
Licence verdict RED (redistribution AND personal                → L
  use outside Unity both forbidden, or origin unknown
  and the publisher refuses)
native-src > 0 AND the DllImport'ed entry points are            → S  (build from source)
  defined in that source (exports.txt / grep agree)
native-src == 0 AND a native DLL exports the DllImport'ed       → D  (prebuilt DLL)
  names AND its dependents do NOT include UnityPlayer.dll /
  call IUnityInterface (else it cannot load outside Unity → P or L)
no native code; algorithm in C# (Burst/Jobs/plain)              → P  (P-a host in .NET first; P-b port to C++)
IL2CPP output only (generated C++ + il2cpp runtime)             → P  (generated code is not maintainable source)
Mixed (source for some, DLL for other entry points)             → S for the source part + D adapter for the rest
Compute shaders carry core steps                                → any branch + R7 (GPU) risk: worker needs a
                                                                  GL/D3D compute context; re-open D2
```

### Downstream milestones per branch

The tasks are defined in `docs/work-breakdown.md`.

| Branch | Milestones, in order | Parity tier target (Windows / Linux) |
|---|---|---|
| **S** source | B1 intake review → S1 build under CMake → S2 ABI doc + boundary capture + adapter → B3 parity harness and calibration → B4 progress/cancel → B5 parameter spec and GUI panel → B6 release | T1 (T0 if Unity's own result is deterministic and our flags match) / T2 |
| **D** prebuilt DLL | B1 → D1 ABI recovery + loadability test → D2 DLL adapter (Windows-only) → B3 → B4 → B5 → B6 (+ written permission to redistribute the DLL, or personal-use-only build) | T0 / not applicable (Linux uses the fake backend only) |
| **P** C#/Burst | B1 → P0 feasibility (UnityEngine API surface used by the algorithm) → **P-a** .NET 8 worker hosting the C# code with shims → B3 → B4 → B5 → B6; P-b C++ port only if P-a fails P0 or is under 0.5× Unity speed | T2 (Burst float modes vs. .NET JIT differ) |
| **L** licence-blocked | L1: stop Quadremesh work. Ship the A-track app with an OSS backend (`oss.quadriflow` or `oss.instantmeshes`; top-level licences BSD-3-style, dependencies still to audit) → L2 quality comparison against the user's Unity outputs (no parity) → B6 | none (quality metrics only) |

All four branches reuse **all** of Part A, so no Part A work is wasted whatever the outcome.

## 5. `docs/intake-report.md` template

The intake session fills this in:

```
# Intake report (date)
Inputs: qr_intake.zip sha256=…, user answers Q1–Q3 (verbatim)
Unity version:            Quadremesh origin/version:            Package id:
Native footprint:         (source files / build system / DLLs + arch + exports / Burst / compute shaders)
Interop surface:          (each DllImport: library, entry point, signature, CallingConvention, StructLayouts)
C# pre/post-processing:   (welding? world-space transform? index format? submesh handling? threading?)
Threading/determinism:    (OpenMP/TBB/std::thread/Jobs; RNG use; unordered containers)
Licence evidence:         (files + quotes) → verdict GREEN / AMBER / RED, and why
Branch:                   S / D / P / L   (decision-tree line that fired)
Open issues for the user:
Next task:                (ID from docs/work-breakdown.md)
```
