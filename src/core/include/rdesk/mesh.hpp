// Polygon mesh data model (docs/architecture.md §2).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rdesk {

// RightHandedYUpCCW is the internal canonical convention (OBJ, viewer).
// UnityLeftHandedYUpCW is the data exactly as Unity holds it.
enum class Convention : std::uint8_t { RightHandedYUpCCW, UnityLeftHandedYUpCW };

const char* to_string(Convention c) noexcept;

struct VertexAttributes {                  // all optional; sizes are multiples of vertex_count
  std::vector<float> normals;              // 3 per vertex
  std::vector<float> tangents;             // 4 per vertex
  std::vector<float> colors;               // 4 per vertex
  std::array<std::vector<float>, 8> uv;    // uv_dim[i] floats per vertex
  std::array<std::uint8_t, 8> uv_dim{};    // 0 = absent, otherwise 2..4
};

struct PolyMesh {
  std::vector<float> positions;             // xyz, float32 (Unity's native precision)
  std::vector<std::uint32_t> face_offsets;  // size F+1 (CSR), face_offsets[0] == 0
  std::vector<std::uint32_t> corners;       // vertex index per face corner
  std::vector<std::uint32_t> face_group;    // empty, or size F: submesh / material id
  VertexAttributes attrs;                   // per vertex (Unity's split-vertex model)
  Convention convention = Convention::RightHandedYUpCCW;

  std::size_t vertex_count() const noexcept { return positions.size() / 3; }
  std::size_t face_count() const noexcept {
    return face_offsets.empty() ? 0 : face_offsets.size() - 1;
  }
  std::size_t corner_count() const noexcept { return corners.size(); }
  // Precondition: f < face_count() and face_offsets is well formed.
  std::size_t face_size(std::size_t f) const noexcept {
    return face_offsets[f + 1] - face_offsets[f];
  }
};

struct BoundingBox {
  bool valid = false;  // false when there is no finite vertex
  std::array<double, 3> min{};
  std::array<double, 3> max{};
  double diagonal() const noexcept;
};

// Over finite vertices only.
BoundingBox bounding_box(const PolyMesh& m);

// Sum over faces of the fan triangulation (corner 0 as pivot) of dot(a, cross(b, c)) / 6, in double.
// Positive for a closed mesh whose face cross products point outward, in *both* conventions
// (see docs/rdm-format.md, "Coordinate conventions"). Triangles with an out-of-range index are skipped.
double signed_volume(const PolyMesh& m);

}  // namespace rdesk
