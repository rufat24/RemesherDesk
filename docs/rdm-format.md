# RDM mesh container, version 1.0

RDM ("RemesherDesk Mesh") is the one binary mesh format this project uses for three jobs:

1. **Golden data from Unity.** `tools/unity/MeshDump.cs` writes raw `Mesh` arrays.
2. **Worker IPC.** The GUI and the worker exchange meshes as RDM files in the job folder.
3. **Test fixtures** under `tests/data/` and `tests/golden/`.

Using one format for all three means one reader to fuzz, and golden files can go straight into the worker.

The executable reference is `tools/rdm/rdm.py` (standard library only). If it and this page disagree, that is a bug, and both must be fixed in the same commit. `tools/unity/tests/RdmWriterTest` together with `rdm.py check-fixture` checks that the C# writer and the Python reader agree (CI job `intake-tools`).

## Layout

All integers and floats are **little-endian**. Floats are IEEE-754 binary32.

```
Header (16 bytes)
  0  char[4]  magic        "RDMF"
  4  u16      major        1   (readers reject other majors)
  6  u16      minor        0   (readers accept any minor; newer minors only add chunk types/keys)
  8  u32      flags        0   (reserved; writers write 0, readers ignore)
 12  u32      chunk_count

Chunk (repeated chunk_count times)
  0  char[4]  tag          ASCII, space-padded (e.g. "UV0 ")
  4  u32      reserved     0
  8  u64      payload_len  bytes, excluding padding
 16  u8[payload_len] payload
     u8[pad]  zero padding so the next chunk starts on an 8-byte boundary
              (pad = (8 - payload_len % 8) % 8, not counted in payload_len)
```

The first chunk **must** be `META`. Readers **must skip** unknown tags, using `payload_len` and the padding. Each tag appears at most once, except `SUBM`, which repeats once per submesh in order.

## Chunks

| Tag | Required | Payload |
|---|---|---|
| `META` | yes, first | UTF-8 JSON object, no BOM (keys below) |
| `VPOS` | yes | `f32[3 * vertex_count]` positions xyz |
| `VNRM` | no | `f32[3 * vertex_count]` normals |
| `VTAN` | no | `f32[4 * vertex_count]` tangents xyzw (w = bitangent sign) |
| `VCOL` | no | `f32[4 * vertex_count]` colours rgba (linear values exactly as `Mesh.GetColors` returns them) |
| `UV0 ` … `UV7 ` | no | `u32 dim (2..4)`, `u32 0`, then `f32[dim * vertex_count]` |
| `SUBM` | `submesh_count` times | see below |
| `COMP` | no | UTF-8 JSON array `[{ "type": "<C# full type name>", "json": <EditorJsonUtility output> }]` |

### `SUBM`

```
 0  u32  topology     1=triangles 2=quads 3=lines 4=linestrip 5=points 16=polygons
 4  i32  base_vertex  informational only: indices below ALREADY include it
 8  u32  index_count
12  u32  face_count   number of entries in face_sizes (polygons only, else 0)
16  u32[index_count]  vertex indices (always 32-bit, even if the Unity mesh used UInt16)
    u32[face_count]   face sizes, each >= 3, summing to index_count (polygons only)
```

These topology codes are **ours**. They are not Unity's `MeshTopology` values (Unity uses Triangles=0, Quads=2, …), and `MeshDump.cs` maps between them explicitly.

### `META` keys

| Key | Required | Meaning |
|---|---|---|
| `format` | yes | `"rdm"` |
| `version` | yes | `"1.0"` |
| `convention` | yes | `"unity_lh_yup_cw"` (raw Unity data) or `"rh_yup_ccw"` (internal and OBJ convention) |
| `vertex_count` | yes | must match `VPOS` |
| `submesh_count` | yes | must match the number of `SUBM` chunks |
| `producer` | no | for example `"tools/unity/MeshDump.cs"` or `"rdesk 0.1.0"` |
| `label`, `name`, `object_name`, `asset_path` | no | provenance |
| `unity_version`, `created_utc`, `is_readable`, `index_format` | no | provenance (`index_format` is the *source* Unity index format) |
| `bounds_center`, `bounds_extents` | no | Unity `Mesh.bounds` |
| `local_to_world_row_major` | no | 16 floats: the owning object's `localToWorldMatrix` |
| `params` | no | backend parameters used to produce this mesh (worker output) |
| `backend`, `backend_version`, `input_sha256` | no | provenance (worker output) |

## Validation rules (all readers)

A reader must reject the file in any of these cases:

- bad magic or major version
- truncated header, chunk header or payload
- `META` missing, not first, or not a JSON object
- a required `META` key is missing
- an unknown `convention`
- a `VPOS`, `VNRM`, `VTAN`, `VCOL` or `UV` chunk whose size does not match `vertex_count`
- a `SUBM` size mismatch
- an index ≥ `vertex_count`
- a triangles, quads or lines index count that is not a multiple of 3, 4 or 2
- polygon face sizes that are < 3 or do not sum to `index_count`
- a non-finite position

A reader must cap allocations by the file size before it allocates. This is why every length is checked against the remaining bytes. Fuzzing (`docs/testing.md`) enforces it.

## Coordinate conventions

`unity_lh_yup_cw` data is stored **exactly** as Unity holds it. Converting to `rh_yup_ccw` does two things:

- Negate X of positions, normals and tangents (tangent `w` is negated too).
- Reverse every polygon's winding while keeping its first corner: `[a,b,c,d] → [a,d,c,b]`.

Both steps are exact in IEEE arithmetic, so a double conversion gives back the original bits (`rdm.py selftest` checks this).

Why both steps are needed: for a closed mesh, the numeric `cross(b-a, c-a)` points outward in raw Unity data *and* in correctly converted data. Signed volume is therefore positive in both cases. If only one of the two steps is applied, the volume goes negative, and the self-test uses this to catch half-conversions. Signed volume **cannot** tell whether a conversion happened at all. The mirror (chirality) has to be checked with an asymmetric fixture.
