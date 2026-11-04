# RemesherDesk — Plan

Goal: a standalone **desktop** app built around the **native remesher** from the Unity plugin
`C:\Users\user\UnityProjects\Quadremesh` (native part only, no Unity runtime).
This plan supersedes the draft in `rufat24/RemesherDektop` (`PLAN.md`), which had no code.

> **Provenance / honesty note.**
> - The Unity project is not reachable from the cloud session. Nothing here asserts anything about the
>   remesher's algorithm, exported functions or data layout. Intake (B0) exists to fix that.
> - The review and revision below came from a subagent run with the `opus` model alias. The tooling
>   does not expose which Opus version served it, so it is **not verified as Opus 5.5**.
> - Not yet verified by us: the PowerShell inventory one-liner (never run), library licence claims
>   (Polyscope, Instant Meshes, libigl, QuadriFlow, CGAL), and the 2x Windows Actions-minutes multiplier.
>   Check each before relying on it.

## Decisions

1. **Work repo is `rufat24/RemesherDesk`.** Retire `RemesherDektop`. **Make RemesherDesk private before
   pushing any Unity material** (it is currently public).
2. **Build order:** core -> worker CLI -> GUI. Minimal OBJ I/O comes first, because the CLI needs it.
   glTF is off the critical path.
3. **Run each remesh in a separate worker process.** The CLI doubles as the worker and reports progress
   as JSON lines on stdout. Cancel means killing the process. This isolates crashes and global state
   and works the same for the source and DLL-only branches.
4. **GUI: Polyscope** (ImGui + GLFW based) plus `nativefiledialog-extended`. Qt and Tauri are dropped.
5. **Golden data = raw Unity `Mesh` arrays**, not Unity-exported OBJ. Unity has no built-in OBJ
   exporter, and exporters bake in their own axis/winding changes. Capture vertices, index format,
   submeshes and topology before and after a remesh with a small Editor script. After intake, capture
   at the `[DllImport]` boundary too, so the native library is tested separately from the C# pipeline.
6. **Convention handling lives only in the I/O layer.** Unity is left-handed, Y-up, clockwise front
   faces: flip X *and* reverse winding. The core stays convention-neutral.

## Part A — Can start now, without the source

| # | Milestone | Exit check | Worth doing before intake? |
|---|---|---|---|
| S1 | CMake skeleton + presets; CI on Linux (gcc, clang) and Windows (MSVC); stub library with ctest | Both CI jobs green | Yes |
| S2 | Mesh model (32-bit indices, polygon faces); OBJ and PLY I/O (tinyobjloader / happly); `UnityDump` reader with explicit left-handed -> right-handed conversion | Round-trip tests pass; signed volume > 0 after conversion | Yes |
| S3 | Unity Editor script `tools/unity/MeshDump.cs` that dumps raw `Mesh` arrays | You send 2 before/after dump pairs | Yes (this is the golden data) |
| S4 | `remesh-worker` CLI with a fake backend (e.g. Catmull-Clark split) and a JSON-lines progress/cancel protocol | Worker is killed within 200 ms of cancel | Yes, minimal version |
| S5 | Polyscope viewer shell: load, run worker, before/after, export | Windows CI artifact launches; Linux Xvfb screenshot smoke test | Thin shell only |

Likely wasted if done before intake: the parameter panel, a C ABI facade / `RemeshParams` struct, glTF,
packaging, and anything assuming the algorithm's inputs (feature edges, target count, density maps).

## Part B — Blocked on the source

| # | Milestone | Exit check |
|---|---|---|
| B0 | Inventory reviewed; branch chosen (C++ source / DLL only / Burst-compiled C#); licence verdict | `docs/abi.md` and `docs/licenses.md` written |
| B1 | Native code builds under our CMake with no Unity headers | Linux and Windows CI green |
| B2 | Backend adapter in the worker + parity tests against the S3 dumps | Windows MSVC: counts identical, positions bitwise equal or within 1e-6 x bbox diagonal. Linux: same counts, Hausdorff < 1e-4 x diagonal, repeat runs identical or the difference explained |
| B3 | Real progress and cancel, if the native API supports them | Progress only increases; cancel works |
| B4 | Parameter panel mirroring the Unity inspector's defaults and ranges | Defaults table matches the C# source |
| B5 | Packaging: zip with licences; CI runs `app --selftest` | Clean Windows runner passes |

If the "native" part is **Burst-compiled C#** or IL2CPP output, there is no C++ source and the job becomes
a port. The inventory (below) detects this. If it is a **DLL only**: Windows x64 only, check
`dumpbin /dependents` for UnityPlayer / `IUnityInterface` (if present it will not load outside Unity),
test only in Windows CI, and confirm the DLL is redistributable.

## Getting the code into the cloud

**Recommended: `git push` to the private RemesherDesk repo**, as an untouched first commit under
`third_party/quadremesh/`, so later changes show as diffs. A zip upload is fine only for the small
inventory zip. Remote Control is not reliably available. Do not use Git LFS (cloud sessions may not
receive LFS content).

Collect: native source + build files (also look outside `Assets/`: `Native~`, `Library/PackageCache`);
shipped binaries in `Plugins/`; every `.cs` containing `DllImport`, plus C# that runs before/after the
native call; the inspector/editor window script; LICENSE/COPYING/NOTICE files and the Asset Store
invoice/EULA if bought; 2-3 raw before/after dumps with their parameters.

**Step 0a — inventory (CSV only, no source).** Run in "Developer PowerShell for VS" so `dumpbin` exists
(plain PowerShell works but skips the exports step). *Untested.*

```powershell
$p='C:\Users\user\UnityProjects\Quadremesh';$o="$env:USERPROFILE\Desktop\qr_intake";mkdir $o -f|out-null;$r=@("$p\Assets","$p\Packages","$p\Library\PackageCache")+(gci $p -dir|?{$_.Name -notin 'Assets','Packages','Library','Temp','Logs','obj','UserSettings','ProjectSettings'}|%{$_.FullName});$f=gci $r -r -file -ea 0;$f|?{$_.Extension -in '.c','.cc','.cpp','.cxx','.h','.hpp','.inl','.dll','.so','.dylib','.bundle','.lib','.a','.def','.vcxproj','.sln' -or $_.Name -match '^(CMakeLists\.txt|LICENSE|COPYING|NOTICE)'}|select FullName,Length|export-csv "$o\native_files.csv" -nti;$f|?{$_.Extension -eq '.cs'}|sls 'DllImport|extern static|BurstCompile' -list|select Path,LineNumber,Line|export-csv "$o\interop_cs.csv" -nti;if(gcm dumpbin -ea 0){$f|?{$_.Extension -eq '.dll'}|%{"== $($_.FullName)";dumpbin /nologo /exports /dependents $_.FullName}|out-file "$o\dll_exports.txt"};Compress-Archive "$o\*" "$o.zip" -f
```

**Step 0b — push the source** (after reviewing 0a and making the repo private):

```powershell
cd C:\src; git clone https://github.com/rufat24/RemesherDesk; cd RemesherDesk; git switch -c intake; robocopy <native dirs> third_party\quadremesh\native /E; robocopy <cs dirs> third_party\quadremesh\unity_cs /E; git add -A; git commit -m "Vendor Quadremesh native + interop (unmodified)"; git push -u origin intake
```

## Open questions (default if unanswered)

1. Is the remesher your own code, open source, or from the Asset Store? *Default: treat as restricted;
   private repo; distribute nothing until the licence is cleared.*
2. Native part: C++ source, DLL only, or Burst C#? *Default: C++ source (the inventory answers this).*
3. For you only, or shipped to others? *Default: you only, Windows only.*
4. Must it match Unity exactly? *Default: tolerance-based parity; bitwise only on Windows MSVC.*
5. File formats? *Default: OBJ + PLY in/out; glTF later; no FBX.*
6. Linux/macOS builds? *Default: no; Linux is for dev and CI only.*

## Risks

- **Licence (can end the project):** Asset Store EULA; GPL parts such as libigl `copyleft/comiso`, CGAL,
  CoMISo; Gurobi-style solvers. Audit at intake, not at packaging.
- **Determinism:** TBB/OpenMP scheduling, seeds, `/fp:fast` vs `-ffast-math`, `unordered_*` order. Exact
  parity is only realistic with the same compiler and flags as the Unity build.
- **Index format:** Unity defaults to 16-bit indices; vertices split at UV/normal seams; submeshes. Find
  out whether the C# welds before the native call and do the same.
- **CI cost:** Windows runners bill at a higher multiplier on private repos; run Windows CI on PRs and
  main, not every push.

## Next 3 actions

1. **You:** make RemesherDesk private; run the inventory one-liner; send `qr_intake.zip`; answer
   questions 1-3.
2. **Next session (before intake):** S1, S2 and the S3 dump script.
3. **After the inventory:** B0 (branch + licence verdict); you push the source to `intake` and run the
   dump script on 2 meshes; the session after that starts B1.
