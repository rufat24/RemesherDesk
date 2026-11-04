# GUI and UX specification

Toolkit: Polyscope (Dear ImGui + GLFW + OpenGL 3.3), pending the A10 spike. The fallback is plain Dear ImGui + GLFW with an in-house GL 3.3 renderer, using the same panel code. Rationale and reversal criteria are in `docs/decisions.md` D3.

The GUI never runs remesh code itself. It launches the `rdesk` worker (`docs/architecture.md` §4).

## 1. Main window layout

```
┌───────────────────────────────────────────────────────────────────────────────┐
│ File  View  Run  Help                                                          │  menu bar
├──────────────────────┬────────────────────────────────────────────────────────┤
│ INPUT                │                                                        │
│  armchair.obj        │                                                        │
│  12,480 v · 24,952 f │                    3D viewport                         │
│  tris 100% · 1 comp  │            (input | output | side-by-side | overlay)   │
│  ⚠ 3 non-manifold e. │                                                        │
│ ─────────────────────│                                                        │
│ BACKEND [Quadremesh▾]│                                                        │
│ PARAMETERS           │                                                        │
│  ▸ Basic             │                                                        │
│  ▸ Advanced          │                                                        │
│  [Reset to defaults] │                                                        │
│  [Load…] [Save…]     │                                                        │
│ ─────────────────────│                                                        │
│ [ Run  (F5) ]        │                                                        │
│ OUTPUT               │                                                        │
│  6,012 v · 6,010 f   │                                                        │
│  quads 99.7% · irr 26│                                                        │
│  Hausdorff 0.21 ē    │                                                        │
│  [Export… (Ctrl+S)]  │                                                        │
├──────────────────────┴────────────────────────────────────────────────────────┤
│ ███████████░░░░░ 61%  remesh · 00:04.2 · [Cancel (Esc)]            [Log ▾]    │  status bar
└───────────────────────────────────────────────────────────────────────────────┘
```

- The left panel is fixed-width (320 px, resizable) and scrolls.
- The log panel slides up from the status bar. It shows the in-memory log with level filters, plus "Copy", "Open job folder" and "Save bug report…".
- Drag and drop: dropping `.obj` or `.rdm` files on the window opens them. If there are several files, the first one is opened and a warning is shown.

## 2. Parameter panel

The panel is **generated from data**. `rdesk backends --json` returns `ParamSpec[]` (`docs/architecture.md` §3), so the panel code knows nothing about Quadremesh.

| ParamType | Widget |
|---|---|
| Bool | checkbox |
| Int / Float with hard `[min,max]` | slider + numeric field (Ctrl+click to type); clamped |
| Int / Float with soft range | drag field; out-of-range values get an amber outline and a tooltip |
| Enum | combo, in C# declaration order |
| String | text field |

- Groups become collapsing headers. `advanced` params are collapsed by default.
- Each widget's tooltip comes from `tooltip`.
- **Reset to defaults** restores the backend's defaults. Changed values show a "●" marker with a per-field reset.
- **Save / Load** writes `params.json`, the same format as `rdesk remesh --params`, so a GUI run can be reproduced from the CLI.
- The last-used parameters per backend are stored in `settings.json`.
- **(B5)** For Quadremesh, the `ParamSpec` table mirrors the Unity inspector: field names, labels (Unity's `ObjectNames.NicifyVariableName` form), defaults, `[Range]`, `[Tooltip]`, enum order and grouping. The table is written at B5 from the C# source. No parameter is assumed before then.

## 3. Viewer features

- **View modes** (keys 1–4):
  1. Input
  2. Output
  3. Side-by-side (output offset by 1.2× the bounding-box width along +X, cameras linked)
  4. Overlay (output wireframe over shaded input)
- **Shading** (key W cycles): shaded, shaded + wireframe, wireframe only. Choose flat or smooth normals.
- **Colour modes** (key C cycles):
  - none
  - **face type**: triangle orange, quad blue, n-gon red
  - **vertex valence / singularities**: valence 3 blue, 5 red, 6+ magenta, boundary irregular grey
  - **distance to input**: heat map, after the run
  - **face group / submesh**
- **Backface colour.** Back faces are tinted, so winding or convention mistakes are visible at once.
- **Problem overlays** from the validation report: non-manifold edges and vertices, boundary loops, degenerate faces. Each can be toggled from the input stats line.
- **Camera:**
  - orbit, pan and zoom; F frames the view
  - ground plane on/off
  - turntable or free rotation
  - the up axis is fixed to Y for both conventions
- **Screenshot** (F12): saves a PNG next to the last export or into Pictures.
- **Performance target:** 1M-triangle input at ≥ 30 fps on a mid-range discrete GPU (UNVERIFIED target, measured in A11).

## 4. Run, progress and cancel

| State | Run button | Progress bar | Cancel |
|---|---|---|---|
| Idle, no input | disabled ("Open a mesh first") | hidden | hidden |
| Idle, input loaded | enabled | hidden / last result | hidden |
| Starting | disabled | indeterminate, "starting worker…" | enabled |
| Running | disabled; params read-only | determinate if `frac` is set, else indeterminate; stage text; elapsed time | enabled (Esc) |
| Cancelling | disabled | "stopping… forcing in 0.6 s" countdown | disabled |
| Succeeded | enabled | full; total time | hidden |
| Failed / Crashed / Cancelled | enabled | red or grey text with the reason | hidden |

- Changing the input or the params after a run marks the output **stale**: it gets a dimmed colour and a "stale" badge. It is not discarded.
- Closing the window while a job runs asks "Cancel the running remesh and quit?". Choosing yes cancels with the normal grace period, then quits.

## 5. Error states

| Situation | Where it shows | Message (example) | Action offered |
|---|---|---|---|
| File unreadable or not found | modal | "Could not open 'x.obj': file not found." | — |
| Parse error | modal | "x.obj line 1532: face index 99812 out of range (12480 vertices)." | open log |
| Input invalid for the backend (exit 4) | input panel, highlighted | "3 non-manifold edges. Quadremesh requires a manifold mesh." | "Show" toggles the overlay. Repair is a non-goal for v1. |
| Parameter invalid (exit 4) | the field, red | the message from the worker | reset field |
| Backend unavailable (exit 7) | banner | "Quadremesh backend not available: QuadRemesh.dll not found next to rdesk.exe." | open folder |
| Backend failed (exit 5) | banner | the worker's message | Save bug report |
| Worker crashed | banner | "The remesher crashed (code 0xC0000005). Your input and parameters are saved in the job folder." | Save bug report, open job folder |
| Forced cancel | status bar | "Cancelled (worker terminated)" | — |
| Output write or export failed | modal | OS error text + path | — |
| OpenGL 3.3 unavailable | native message box before the window opens | "RemesherDesk needs OpenGL 3.3. Update your graphics driver. The command-line tool `rdesk` works without a GPU." | — |
| Settings corrupt | log warning | "settings.json was invalid and has been reset (backup: settings.json.bad)." | — |

## 6. Keyboard shortcuts

Shortcuts are ignored while a text field has focus (ImGui `io.WantTextInput`).

| Key | Action |
|---|---|
| Ctrl+O | Open mesh |
| Ctrl+S / Ctrl+Shift+S | Export output / Export as… |
| F5 or Ctrl+Enter | Run |
| Esc | Cancel the running job; otherwise close the modal |
| 1 / 2 / 3 / 4 | Input / Output / Side-by-side / Overlay |
| W | Cycle shading |
| C | Cycle colour mode |
| F | Frame the view |
| G | Toggle ground plane |
| Ctrl+L | Toggle log panel |
| F12 | Screenshot |
| Ctrl+, | Preferences |
| Ctrl+Q | Quit |

## 7. Other screens

- **Preferences:**
  - cancel grace time (250–10000 ms)
  - worker path override (developers)
  - job folder location and how many to keep
  - OBJ import convention (RH / Unity LH)
  - UI scale (100–200%, for high-DPI displays)
  - theme (dark / light)
- **About:** version, the backend's version, and a "Third-party licences" viewer that shows `THIRD_PARTY_NOTICES.txt`.

## 8. GUI acceptance checklist (A11 exit; repeated at B6 with the real backend)

1. Open each `tests/data` OBJ; stats are correct.
2. Run the fake backend; the progress bar moves.
3. Run with `debug.sleep_ms_per_iter=500` and cancel: idle again in ≤ 1.25 s.
4. Run with `debug.hang` and cancel: forced, idle in ≤ 1.25 s, message shown.
5. Run with `debug.crash`: the crash banner appears and "Save bug report" produces a zip.
6. Try all four view modes and all colour modes.
7. Export OBJ, then re-open it: identical stats.
8. Save and load params JSON; `rdesk remesh --params` gives an identical `output_sha256`.
9. Use every shortcut in §6.
10. Corrupt `settings.json`: the app recovers.
11. Resize the window, minimise and restore: no crash; the viewport has the correct aspect.
