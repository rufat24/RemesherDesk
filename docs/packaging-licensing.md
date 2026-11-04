# Packaging, distribution and licensing

> Nothing in this document is legal advice. Every statement about a licence or EULA that has not been checked against the actual licence text is marked UNVERIFIED.

## 1. Licence audit procedure (runs in B1; repeat at B6)

1. **Collect evidence.** Use `qr_intake.zip` → `meta/` (all LICENSE, COPYING, NOTICE, README and `package.json` files). After the source is pushed, also run:
   ```bash
   grep -rIn -iE "copyright|licen[cs]e|SPDX-License-Identifier|all rights reserved|GPL|LGPL|MPL|Apache" third_party/quadremesh
   ```
2. **Establish the origin of Quadremesh.** It is one of:
   - the user's own code
   - an Asset Store purchase (keep the invoice and the asset page URL)
   - open source (keep the repository URL and the commit)
   - mixed
3. **Find embedded third-party code.** Look for:
   - its own headers and namespaces (`Eigen/`, `OpenMesh`, `igl::`, `COMISO`, `CGAL`, `vcg::`, `geogram`, `tbb`)
   - files copied from known projects (compare file names against the matrix below)
   - solver calls to commercial solvers such as Gurobi or MOSEK, which need runtime licences
4. **Make a verdict per component.** The question is: may we (a) use it outside Unity for personal use, (b) redistribute it as source, (c) redistribute it as a binary?
5. **Record the result** in `docs/licenses.md`. For each component give: name, version, origin, licence, the evidence path quoted, the obligations (notice, source offer, copyleft scope), and the verdict.
6. **Overall verdict:**
   - **GREEN:** everything is redistributable with notices.
   - **AMBER:** personal use only, or the publisher's written permission is needed.
   - **RED:** not even personal use outside Unity, or origin unknown. RED leads to branch L.

## 2. Third-party licence matrix

The "Top-level licence" column was **read from the repository's LICENSE file on its default branch on 2026-11-04** in the planning session. Anything else is UNVERIFIED. Sub-components and vendored dependencies were **not** checked. At intake, re-check the **exact version** that is actually used.

### Candidate quad-remeshing and geometry codebases (what Quadremesh might contain, or what branch L might use)

| Project | Top-level licence (checked) | What to check at intake |
|---|---|---|
| Instant Meshes (wjakob/instant-meshes) | BSD-3-style ("Redistribution and use in source and binary forms…") | Bundled `ext/` dependencies (nanogui, TBB, …), UNVERIFIED |
| QuadriFlow (hjwdzh/QuadriFlow) | BSD-3-style | Optional solver dependencies (LEMON (Boost licence?), Boost, Gurobi?), UNVERIFIED |
| libigl | MPL-2.0 for the core (`LICENSE.MPL2`), plus a `LICENSE.GPL` file in the repository | Which modules are used. `copyleft/` (CoMISo, CGAL wrappers) is GPL: **contaminates a binary**. MPL-2.0 is file-level copyleft: modified MPL files must be offered as source |
| CoMISo | GPL-3 (UNVERIFIED) | Often reached via libigl `copyleft/comiso`, used for mixed-integer quadrangulation |
| QuadWild (nicopietroni/quadwild) | GPL-3 | GPL-3: binary distribution needs full source under GPL |
| QuadWild-BiMDF (cgg-bern/quadwild-bimdf) | GPL-3 | same |
| VCGlib (cnr-isti-vclab/vcglib) | GPL-3 | used by QuadWild and MeshLab-derived code |
| CGAL | mixed GPL/LGPL per package, with commercial licences available (per its `LICENSE.md`) | which packages are used |
| Geogram (BrunoLevy/geogram) | BSD-3 | — |
| OpenMesh | BSD-3 (UNVERIFIED; hosted on RWTH GitLab, not checked) | — |
| pmp-library | MIT | — |
| Eigen | MPL-2.0 (UNVERIFIED; GitLab, not checked) | any `EIGEN_MPL2_ONLY` define |
| Directional (avaxman/Directional) | not checked (LICENSE not found at the guessed path) | — |
| Unity native plugin API headers (`IUnityInterface.h`, …) | UNVERIFIED | redistributability of the headers |
| Unity.Mathematics / Collections / Burst (branch P) | UNVERIFIED (Unity Companion License expected) | whether use outside Unity is allowed |

### Our own v1 dependencies

| Dependency | Top-level licence (checked) | Obligation |
|---|---|---|
| Polyscope | MIT | include notice |
| Dear ImGui (via Polyscope) | MIT | include notice |
| GLFW (via Polyscope) | zlib | none for binaries (keep notice) |
| nativefiledialog-extended | zlib | same |
| tinyobjloader | MIT | include notice |
| nlohmann/json 3.11.3 (pinned to commit `9cca280…`) | MIT (LICENSE.MIT read at that tag) | include notice |
| doctest 2.4.11 (tests only, not shipped; pinned to commit `ae7a135…`) | MIT (LICENSE.txt read at that tag) | — |
| happly (optional A15) | MIT | include notice |
| MSVC runtime (app-local) | Microsoft redistribution terms (UNVERIFIED which files are allowed) | follow the VS "Distributable Code" list |

## 3. Unity Asset Store considerations (UNVERIFIED: read the current text)

If Quadremesh was bought on the Asset Store, its licence is governed by the Unity Asset Store Terms of Service and EULA in force for that purchase, plus any licence file the publisher includes. The intake session must quote the relevant clauses from the actual text. General points to check:

- **Redistribution.** Asset Store EULAs commonly forbid redistributing an asset's source or binaries *on a standalone basis*. A desktop app that bundles the remesher is likely to fall under that. UNVERIFIED.
- **Use outside the Unity Editor or runtime.** Check whether the licence restricts use to Unity projects. Some "Extension Asset" or tool licences are per-seat and tied to the Editor. UNVERIFIED.
- **Seats.** A per-seat licence may allow the user, and only the user, to run it.
- **Publisher permission.** A written email permission from the publisher is the robust way to turn an AMBER verdict into GREEN.

How each verdict changes packaging:

| Verdict | Repository | Builds | Distribution |
|---|---|---|---|
| GREEN | may be public, with licences | CI artifacts are fine | public releases with notices |
| AMBER (personal use) | **private** | CI artifacts are fine (private), 7-day retention | **no** releases; the user downloads from private CI or builds locally |
| RED | private; Quadremesh material removed from history if the publisher requires it | branch L backend only | releases with the OSS backend only |

## 4. Packaging (A13, finalised in B6)

- **v1 format: a portable zip.** `RemesherDesk-<version>-win64.zip` contains:
  - `RemesherDesk.exe` (GUI)
  - `rdesk.exe` (worker/CLI)
  - the backend DLL(s), if branch D and the licence allows
  - the MSVC runtime DLLs, app-local, via CMake `InstallRequiredSystemLibraries`
  - `README.txt` (quick start, where logs and jobs live)
  - `THIRD_PARTY_NOTICES.txt`
  - `licenses/` (full texts)

  It needs no installer and no admin rights, and it runs from any folder. The user's settings, logs and jobs live in `%APPDATA%` / `%LOCALAPPDATA%`, never next to the exe.
- **Runtime linkage:** `/MD` plus app-local CRT, not `/MT`. A vendored or prebuilt Quadremesh DLL probably expects the shared CRT, and mixing CRTs across a DLL boundary that passes heap memory is a classic bug.
- **Installer (later, only if wanted):** Inno Setup (free) or WiX v4. Choose it when the user wants Start-menu entries or `.obj` file associations.
- **Code signing:**
  - v1 is unsigned. Windows SmartScreen will warn ("More info → Run anyway"), which is acceptable for personal use.
  - For distribution to others, use Azure Trusted Signing or an OV certificate (cost and eligibility UNVERIFIED). Sign both exes and the installer in CI with secrets stored as environment-protected GitHub secrets.
- **Updates:**
  - No auto-updater in v1.
  - Version is shown in About. The release notes are the Git tag message plus `CHANGELOG.md`.
  - Later, an optional "Check for updates" could read the GitHub Releases API, but only if the repo is public (GREEN).
- **Versioning:** SemVer from `project(VERSION)` in CMake. Tags are `v0.x.y`. The worker protocol version and the RDM major version are independent integers; see `rdesk/version.hpp`.
