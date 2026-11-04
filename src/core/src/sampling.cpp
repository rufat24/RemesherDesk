#include <algorithm>
#include <cmath>

#include "aabb_tree.hpp"
#include "geometry.hpp"
#include "pcg32.hpp"
#include "rdesk/metrics.hpp"
#include "rdesk/validate.hpp"

namespace rdesk {

namespace {

struct FanTri {
  std::uint32_t a, b, c;
};

double tri_area(const float* a, const float* b, const float* c) noexcept {
  const double ux = double(b[0]) - a[0], uy = double(b[1]) - a[1], uz = double(b[2]) - a[2];
  const double vx = double(c[0]) - a[0], vy = double(c[1]) - a[1], vz = double(c[2]) - a[2];
  const double x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
  return 0.5 * std::sqrt(x * x + y * y + z * z);
}

Result<void> check_usable(const PolyMesh& m, const char* which) {
  if (auto fe = format_errors(m); !fe.empty()) {
    return make_error(Errc::InputInvalid, std::string(which) + ": malformed mesh container: " + fe.front());
  }
  for (const std::uint32_t c : m.corners) {
    if (c >= m.vertex_count()) return make_error(Errc::InputInvalid, std::string(which) + ": corner index out of range");
  }
  bool any = false;
  for (std::size_t f = 0; f < m.face_count(); ++f) any = any || m.face_size(f) >= 3;
  if (!any) return make_error(Errc::InputInvalid, std::string(which) + ": the mesh has no face with 3 or more corners");
  return {};
}

struct Sampler {
  std::vector<FanTri> tris;
  std::vector<double> cumulative;  // inclusive prefix sums of triangle areas
  double total = 0;
  explicit Sampler(const PolyMesh& m) {
    for (std::size_t f = 0; f < m.face_count(); ++f) {
      const std::uint32_t b = m.face_offsets[f], n = m.face_offsets[f + 1] - b;
      for (std::uint32_t k = 1; k + 1 < n; ++k) tris.push_back({m.corners[b], m.corners[b + k], m.corners[b + k + 1]});
    }
    cumulative.reserve(tris.size());
    for (const FanTri& t : tris) {
      total += tri_area(&m.positions[3 * std::size_t{t.a}], &m.positions[3 * std::size_t{t.b}], &m.positions[3 * std::size_t{t.c}]);
      cumulative.push_back(total);
    }
  }
};

void sample_point(const PolyMesh& m, const Sampler& s, detail::Pcg32& rng, double* out) {
  // Pick a triangle proportionally to its area, then a point uniformly inside it.
  const double r = rng.next_double() * s.total;
  std::size_t i = static_cast<std::size_t>(std::upper_bound(s.cumulative.begin(), s.cumulative.end(), r) - s.cumulative.begin());
  if (i >= s.tris.size()) i = s.tris.size() - 1;
  const double r1 = rng.next_double(), r2 = rng.next_double();
  const double u = std::sqrt(r1);
  const double wa = 1.0 - u, wb = u * (1.0 - r2), wc = u * r2;
  const FanTri& t = s.tris[i];
  const float* a = &m.positions[3 * std::size_t{t.a}];
  const float* b = &m.positions[3 * std::size_t{t.b}];
  const float* c = &m.positions[3 * std::size_t{t.c}];
  for (int k = 0; k < 3; ++k) out[k] = wa * a[k] + wb * b[k] + wc * c[k];
}

OneSidedDistance one_sided(const PolyMesh& from, const PolyMesh& to, const detail::TriangleTree& tree,
                           const DistanceOptions& opts) {
  OneSidedDistance d;
  const Sampler s(from);
  const std::size_t n = std::max(opts.min_samples, opts.samples_per_face * from.face_count());
  detail::Pcg32 rng(opts.seed);
  double sum = 0, sum_sq = 0, mx = 0;
  if (s.total > 0) {
    for (std::size_t i = 0; i < n; ++i) {
      double p[3];
      sample_point(from, s, rng, p);
      const double d2 = tree.distance_sq(p);
      sum += std::sqrt(d2);
      sum_sq += d2;
      mx = std::max(mx, d2);
    }
    d.samples = n;
    d.mean = sum / static_cast<double>(n);
    d.rms = std::sqrt(sum_sq / static_cast<double>(n));
  }
  if (opts.include_vertices) {
    std::vector<std::uint8_t> used(from.vertex_count(), 0);
    for (const std::uint32_t c : from.corners) used[c] = 1;
    for (std::size_t v = 0; v < used.size(); ++v) {
      if (!used[v]) continue;
      const double p[3] = {from.positions[3 * v], from.positions[3 * v + 1], from.positions[3 * v + 2]};
      mx = std::max(mx, tree.distance_sq(p));
    }
  }
  d.max = std::sqrt(mx);
  (void)to;
  return d;
}

}  // namespace

std::vector<double> sample_surface(const PolyMesh& m, std::size_t count, std::uint64_t seed) {
  std::vector<double> pts;
  if (!check_usable(m, "mesh")) return pts;
  const Sampler s(m);
  if (s.total <= 0) return pts;
  detail::Pcg32 rng(seed);
  pts.resize(count * 3);
  for (std::size_t i = 0; i < count; ++i) sample_point(m, s, rng, &pts[3 * i]);
  return pts;
}

double DistanceReport::rms() const noexcept {
  const double a = a_to_b.rms, b = b_to_a.rms;
  return std::sqrt(0.5 * (a * a + b * b));
}

Result<DistanceReport> surface_distance(const PolyMesh& a, const PolyMesh& b, const DistanceOptions& opts) {
  if (auto r = check_usable(a, "first mesh"); !r) return r.error();
  if (auto r = check_usable(b, "second mesh"); !r) return r.error();
  const detail::TriangleTree ta(a), tb(b);
  DistanceReport rep;
  rep.a_to_b = one_sided(a, b, tb, opts);
  rep.b_to_a = one_sided(b, a, ta, opts);
  rep.max_quad_nonplanarity = std::max(detail::max_quad_nonplanarity(a), detail::max_quad_nonplanarity(b));
  return rep;
}

}  // namespace rdesk
