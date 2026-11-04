// Exact Unity <-> right-handed conversion (docs/rdm-format.md, "Coordinate conventions").
#pragma once

#include "rdesk/mesh.hpp"

namespace rdesk {

// Converts in place. A no-op when `m.convention == target`.
// Unity <-> RH is the same involution in both directions:
//   - negate X of positions, normals and tangents (tangent.w is negated too);
//   - reverse every polygon's winding while keeping its first corner: [a,b,c,d] -> [a,d,c,b].
// Both steps are exact in IEEE arithmetic, so converting twice returns the original bits.
// UVs, colours, face_group and the vertex order are untouched.
void convert_in_place(PolyMesh& m, Convention target) noexcept;

// Returns a converted copy.
PolyMesh to_convention(const PolyMesh& m, Convention target);

}  // namespace rdesk
