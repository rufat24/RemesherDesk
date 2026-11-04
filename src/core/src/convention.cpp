#include "rdesk/convention.hpp"

#include <algorithm>

namespace rdesk {

namespace {
void negate_component(std::vector<float>& v, std::size_t stride, std::size_t comp) noexcept {
  for (std::size_t i = comp; i < v.size(); i += stride) v[i] = -v[i];
}
}  // namespace

void convert_in_place(PolyMesh& m, Convention target) noexcept {
  if (m.convention == target) return;

  negate_component(m.positions, 3, 0);
  negate_component(m.attrs.normals, 3, 0);
  negate_component(m.attrs.tangents, 4, 0);
  negate_component(m.attrs.tangents, 4, 3);

  // [c0, c1, ..., c(n-1)] -> [c0, c(n-1), ..., c1]
  const std::size_t nf = m.face_count();
  for (std::size_t f = 0; f < nf; ++f) {
    const std::size_t b = m.face_offsets[f];
    const std::size_t e = std::min<std::size_t>(m.face_offsets[f + 1], m.corners.size());
    if (e > b + 2) {
      std::reverse(m.corners.begin() + static_cast<std::ptrdiff_t>(b + 1),
                   m.corners.begin() + static_cast<std::ptrdiff_t>(e));
    }
  }
  m.convention = target;
}

PolyMesh to_convention(const PolyMesh& m, Convention target) {
  PolyMesh out = m;
  convert_in_place(out, target);
  return out;
}

}  // namespace rdesk
