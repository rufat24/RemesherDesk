// Wavefront OBJ import / export.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "rdesk/mesh.hpp"
#include "rdesk/result.hpp"

namespace rdesk::io {

struct ObjReadOptions {
  // The convention the file's coordinates are in. OBJ files are right-handed by default. The data is
  // NOT converted: `mesh.convention` just records this, exactly like the RDM reader. Convert with
  // `to_convention` when needed.
  Convention source_convention = Convention::RightHandedYUpCCW;
};

struct ObjFile {
  // Polygons are kept as they are (no triangulation). Per-corner `vt` / `vn` are split into unique
  // vertices (v, vt, vn), numbered in ascending (v, vt, vn) order, so a file without `vt` / `vn` keeps its
  // `v` numbering and every `v` line becomes a vertex even when no face uses it. The first texture
  // coordinate set goes to uv[0] (dim 2); `vn` goes to normals. Vertex colours (`v x y z r g b`) are ignored.
  // Vertices without a `vt` / `vn` get zeros when other vertices have one (a warning is added).
  PolyMesh mesh;
  // `face_group[f]` indexes this list: one entry per distinct (`o`/`g` name, `usemtl` name) pair, in order
  // of first appearance. The label is the shape name, plus "|<material>" when a material is in effect.
  std::vector<std::string> group_labels;
  std::vector<std::string> warnings;
};

// Errc::Parse for syntax errors, out-of-range indices and anything tinyobjloader rejects.
Result<ObjFile> read_obj(std::string_view text, const ObjReadOptions& opts = {});
Result<ObjFile> read_obj_file(const std::filesystem::path& path, const ObjReadOptions& opts = {});

struct ObjWriteOptions {
  Convention target = Convention::RightHandedYUpCCW;  // the mesh is converted (as a copy) before writing
  const std::vector<std::string>* group_labels = nullptr;  // optional; "group_<id>" otherwise
  std::string comment;                                      // written as "# ..." lines at the top
};

// Floats are written with the shortest representation that reads back to the same bits
// (std::to_chars), so the output does not depend on the C locale. `\n` line endings. Faces are written
// group by group (ascending group id, original order inside a group); `g` lines only when face_group is
// non-empty. `vt` / `vn` are written per vertex (uv[0] and normals), and every `f` corner uses the same
// index for v, vt and vn.
// Refuses (Errc::InputInvalid) meshes with a malformed container, out-of-range corner indices or
// non-finite positions / attributes.
Result<std::string> serialize_obj(const PolyMesh& mesh, const ObjWriteOptions& opts = {});
Result<void> write_obj_file(const std::filesystem::path& path, const PolyMesh& mesh, const ObjWriteOptions& opts = {});

}  // namespace rdesk::io
