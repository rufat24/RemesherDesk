// RDM mesh container reader / writer (docs/rdm-format.md, version 1.0).
// tools/rdm/rdm.py is the executable reference; keep the two in agreement.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rdesk/mesh.hpp"
#include "rdesk/result.hpp"

namespace rdesk::io {

struct RdmFile {
  // Faces of all triangle / quad / polygon submeshes, in file order. `face_group[f]` is the index of the
  // submesh the face came from (always populated). `mesh.convention` comes from META.
  PolyMesh mesh;
  std::string meta_json;        // the META object verbatim
  std::string components_json;  // the COMP chunk verbatim, empty if absent
  // Lines / linestrip / point submeshes are skipped, never converted; each one adds a warning.
  std::vector<std::string> warnings;
};

// Rejects, with Errc::Parse, everything listed under "Validation rules" in the spec. Every length is
// checked against the remaining bytes before anything is allocated. Unknown chunks are skipped.
Result<RdmFile> read_rdm(const std::uint8_t* data, std::size_t size);
Result<RdmFile> read_rdm_file(const std::filesystem::path& path);  // Errc::IoRead when unreadable

struct RdmWriteOptions {
  std::string producer = "rdesk";
  // Optional JSON object text merged into META (provenance, params, ...). The keys format, version,
  // convention, vertex_count and submesh_count always come from the writer.
  std::string extra_meta_json;
  std::string components_json;  // optional COMP payload (JSON array text)
};

// One submesh per group id 0..max(face_group) (empty groups are written as empty triangle submeshes, so
// ids survive a round trip); a single submesh when face_group is empty. A submesh is TRIANGLES when all its
// faces are triangles, QUADS when all are quads, POLYGONS otherwise.
// Refuses (Errc::InputInvalid) meshes the reader would reject: bad container, index out of range,
// face with fewer than 3 corners, non-finite position, more than 65536 groups.
Result<std::vector<std::uint8_t>> serialize_rdm(const PolyMesh& mesh, const RdmWriteOptions& opts = {});
// Writes to "<path>.tmp" and renames, so a reader never sees a half-written file.
Result<void> write_rdm_file(const std::filesystem::path& path, const PolyMesh& mesh,
                            const RdmWriteOptions& opts = {});

}  // namespace rdesk::io
