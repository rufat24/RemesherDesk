# RemesherDesk

A standalone Windows desktop app for quad remeshing, built around the native remesher of the Unity plugin *Quadremesh* (no Unity runtime).

**Status:** planning and scaffold. Start with [docs/PLAN.md](docs/PLAN.md).

```bash
cmake --preset ci-gcc && cmake --build --preset ci-gcc && ctest --preset ci-gcc
```

Intake tools for the user's machine:

- `tools/intake/Inventory.ps1`: inventory of the Unity project's native and interop footprint; it copies no source.
- `tools/unity/MeshDump.cs`: Unity Editor menu item that dumps raw meshes to `.rdm` golden files.
- `tools/rdm/rdm.py`: reference reader for `.rdm` files; it can also convert them to OBJ.
