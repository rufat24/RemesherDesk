#include "rdesk/mesh.hpp"

#include <algorithm>
#include <cmath>

#include "rdesk/result.hpp"

namespace rdesk {

const char* to_string(Errc code) noexcept {
  switch (code) {
    case Errc::Usage: return "usage";
    case Errc::IoRead: return "io_read";
    case Errc::Parse: return "parse";
    case Errc::InputInvalid: return "input_invalid";
    case Errc::ParamInvalid: return "param_invalid";
    case Errc::BackendUnavailable: return "backend_unavailable";
    case Errc::BackendFailed: return "backend_failed";
    case Errc::Cancelled: return "cancelled";
    case Errc::IoWrite: return "io_write";
    case Errc::OutputInvalid: return "output_invalid";
    case Errc::Internal: return "internal";
  }
  return "internal";
}

const char* to_string(Convention c) noexcept {
  switch (c) {
    case Convention::RightHandedYUpCCW: return "rh_yup_ccw";
    case Convention::UnityLeftHandedYUpCW: return "unity_lh_yup_cw";
  }
  return "rh_yup_ccw";
}

double BoundingBox::diagonal() const noexcept {
  if (!valid) return 0.0;
  const double dx = max[0] - min[0];
  const double dy = max[1] - min[1];
  const double dz = max[2] - min[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

BoundingBox bounding_box(const PolyMesh& m) {
  BoundingBox bb;
  const std::size_t n = m.vertex_count();
  for (std::size_t i = 0; i < n; ++i) {
    const double p[3] = {m.positions[3 * i], m.positions[3 * i + 1], m.positions[3 * i + 2]};
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
    if (!bb.valid) {
      bb.valid = true;
      for (int k = 0; k < 3; ++k) bb.min[static_cast<std::size_t>(k)] = bb.max[static_cast<std::size_t>(k)] = p[k];
    } else {
      for (std::size_t k = 0; k < 3; ++k) {
        bb.min[k] = std::min(bb.min[k], p[k]);
        bb.max[k] = std::max(bb.max[k], p[k]);
      }
    }
  }
  return bb;
}

double signed_volume(const PolyMesh& m) {
  const std::size_t nv = m.vertex_count();
  const std::size_t nf = m.face_count();
  double vol = 0.0;
  for (std::size_t f = 0; f < nf; ++f) {
    const std::size_t b = m.face_offsets[f];
    const std::size_t e = m.face_offsets[f + 1];
    if (e > m.corners.size() || e < b + 3) continue;
    const std::uint32_t i0 = m.corners[b];
    if (i0 >= nv) continue;
    const float* a = &m.positions[3 * static_cast<std::size_t>(i0)];
    for (std::size_t c = b + 1; c + 1 < e; ++c) {
      const std::uint32_t i1 = m.corners[c];
      const std::uint32_t i2 = m.corners[c + 1];
      if (i1 >= nv || i2 >= nv) continue;
      const float* p = &m.positions[3 * static_cast<std::size_t>(i1)];
      const float* q = &m.positions[3 * static_cast<std::size_t>(i2)];
      const double ax = a[0], ay = a[1], az = a[2];
      const double bx = p[0], by = p[1], bz = p[2];
      const double cx = q[0], cy = q[1], cz = q[2];
      // a . (b x c)
      vol += ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx);
    }
  }
  return vol / 6.0;
}

}  // namespace rdesk
