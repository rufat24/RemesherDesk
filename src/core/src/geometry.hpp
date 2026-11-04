// Internal geometry helpers shared by metrics and sampling.
#pragma once

#include <algorithm>
#include <cmath>

#include "rdesk/mesh.hpp"

namespace rdesk::detail {

// Distance of corner 3 from the plane through corners 0, 1, 2 (0 for degenerate planes).
inline double quad_nonplanarity(const float* p0, const float* p1, const float* p2, const float* p3) noexcept {
  const double ux = double(p1[0]) - p0[0], uy = double(p1[1]) - p0[1], uz = double(p1[2]) - p0[2];
  const double vx = double(p2[0]) - p0[0], vy = double(p2[1]) - p0[1], vz = double(p2[2]) - p0[2];
  const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
  const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (!(len > 0)) return 0.0;
  const double wx = double(p3[0]) - p0[0], wy = double(p3[1]) - p0[1], wz = double(p3[2]) - p0[2];
  return std::fabs((nx * wx + ny * wy + nz * wz) / len);
}

inline double max_quad_nonplanarity(const PolyMesh& m) noexcept {
  double worst = 0;
  for (std::size_t f = 0; f < m.face_count(); ++f) {
    if (m.face_size(f) != 4) continue;
    const std::uint32_t* c = &m.corners[m.face_offsets[f]];
    worst = std::max(worst, quad_nonplanarity(&m.positions[3 * std::size_t{c[0]}], &m.positions[3 * std::size_t{c[1]}],
                                              &m.positions[3 * std::size_t{c[2]}], &m.positions[3 * std::size_t{c[3]}]));
  }
  return worst;
}

}  // namespace rdesk::detail
