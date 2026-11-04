#include "aabb_tree.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace rdesk::detail {

namespace {
inline void sub(const double* a, const double* b, double* o) noexcept {
  o[0] = a[0] - b[0];
  o[1] = a[1] - b[1];
  o[2] = a[2] - b[2];
}
inline double dot(const double* a, const double* b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
constexpr std::uint32_t kLeafSize = 4;
}  // namespace

double point_triangle_distance_sq(const double* p, const double* a, const double* b, const double* c) noexcept {
  double ab[3], ac[3], ap[3];
  sub(b, a, ab);
  sub(c, a, ac);
  sub(p, a, ap);
  const double d1 = dot(ab, ap), d2 = dot(ac, ap);
  double q[3];
  auto dist = [&](const double* cp) {
    const double d[3] = {p[0] - cp[0], p[1] - cp[1], p[2] - cp[2]};
    return dot(d, d);
  };
  if (d1 <= 0 && d2 <= 0) return dist(a);
  double bp[3];
  sub(p, b, bp);
  const double d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return dist(b);
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const double v = d1 / (d1 - d3);
    for (int i = 0; i < 3; ++i) q[i] = a[i] + v * ab[i];
    return dist(q);
  }
  double cp_[3];
  sub(p, c, cp_);
  const double d5 = dot(ab, cp_), d6 = dot(ac, cp_);
  if (d6 >= 0 && d5 <= d6) return dist(c);
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const double w = d2 / (d2 - d6);
    for (int i = 0; i < 3; ++i) q[i] = a[i] + w * ac[i];
    return dist(q);
  }
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    for (int i = 0; i < 3; ++i) q[i] = b[i] + w * (c[i] - b[i]);
    return dist(q);
  }
  const double denom = 1.0 / (va + vb + vc);
  const double v = vb * denom, w = vc * denom;
  for (int i = 0; i < 3; ++i) q[i] = a[i] + ab[i] * v + ac[i] * w;
  return dist(q);
}

TriangleTree::TriangleTree(const PolyMesh& mesh) : mesh_(&mesh) {
  const std::size_t nf = mesh.face_count();
  for (std::size_t f = 0; f < nf; ++f) {
    const std::uint32_t b = mesh.face_offsets[f], n = mesh.face_offsets[f + 1] - b;
    if (n < 3) continue;
    const std::uint32_t* c = &mesh.corners[b];
    if (n == 4) {
      tris_.push_back({c[0], c[1], c[2]});
      tris_.push_back({c[0], c[2], c[3]});
      tris_.push_back({c[1], c[2], c[3]});
      tris_.push_back({c[1], c[3], c[0]});
    } else {
      for (std::uint32_t k = 1; k + 1 < n; ++k) tris_.push_back({c[0], c[k], c[k + 1]});
    }
  }
  if (tris_.empty()) return;
  nodes_.reserve(tris_.size() / 2 + 1);
  build(0, static_cast<std::uint32_t>(tris_.size()));
}

void TriangleTree::tri_vertex(std::uint32_t tri, int k, double* out) const noexcept {
  const float* p = &mesh_->positions[3 * static_cast<std::size_t>(tris_[tri][static_cast<std::size_t>(k)])];
  out[0] = p[0];
  out[1] = p[1];
  out[2] = p[2];
}

std::uint32_t TriangleTree::build(std::uint32_t lo, std::uint32_t hi) {
  const auto id = static_cast<std::uint32_t>(nodes_.size());
  nodes_.emplace_back();
  double bmin[3] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()};
  double bmax[3] = {-bmin[0], -bmin[1], -bmin[2]};
  double cmin[3] = {bmin[0], bmin[1], bmin[2]}, cmax[3] = {bmax[0], bmax[1], bmax[2]};
  for (std::uint32_t t = lo; t < hi; ++t) {
    double cen[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
      double v[3];
      tri_vertex(t, k, v);
      for (int i = 0; i < 3; ++i) {
        bmin[i] = std::min(bmin[i], v[i]);
        bmax[i] = std::max(bmax[i], v[i]);
        cen[i] += v[i];
      }
    }
    for (int i = 0; i < 3; ++i) {
      cen[i] /= 3.0;
      cmin[i] = std::min(cmin[i], cen[i]);
      cmax[i] = std::max(cmax[i], cen[i]);
    }
  }
  Node node;
  for (int i = 0; i < 3; ++i) {
    node.bmin[i] = bmin[i];
    node.bmax[i] = bmax[i];
  }
  if (hi - lo <= kLeafSize) {
    node.first = lo;
    node.count = hi - lo;
    nodes_[id] = node;
    return id;
  }
  int axis = 0;
  for (int i = 1; i < 3; ++i) {
    if (cmax[i] - cmin[i] > cmax[axis] - cmin[axis]) axis = i;
  }
  const std::uint32_t mid = lo + (hi - lo) / 2;
  auto centroid = [&](const std::array<std::uint32_t, 3>& t) {
    double s = 0;
    for (std::size_t k = 0; k < 3; ++k) s += mesh_->positions[3 * static_cast<std::size_t>(t[k]) + static_cast<std::size_t>(axis)];
    return s;
  };
  // Total order (centroid, then vertex indices) so that the split does not depend on the STL's nth_element.
  std::nth_element(tris_.begin() + lo, tris_.begin() + mid, tris_.begin() + hi,
                   [&](const std::array<std::uint32_t, 3>& x, const std::array<std::uint32_t, 3>& y) {
                     const double cx = centroid(x), cy = centroid(y);
                     if (cx != cy) return cx < cy;
                     return x < y;
                   });
  node.left = build(lo, mid);
  node.right = build(mid, hi);
  nodes_[id] = node;
  return id;
}

namespace {
inline double box_distance_sq(const double* bmin, const double* bmax, const double* p) noexcept {
  double s = 0;
  for (int i = 0; i < 3; ++i) {
    const double d = std::max({bmin[i] - p[i], 0.0, p[i] - bmax[i]});
    s += d * d;
  }
  return s;
}
}  // namespace

void TriangleTree::query(std::uint32_t id, const double* p, double& best) const noexcept {
  const Node& n = nodes_[id];
  if (n.count > 0) {
    for (std::uint32_t t = n.first; t < n.first + n.count; ++t) {
      double a[3], b[3], c[3];
      tri_vertex(t, 0, a);
      tri_vertex(t, 1, b);
      tri_vertex(t, 2, c);
      best = std::min(best, point_triangle_distance_sq(p, a, b, c));
    }
    return;
  }
  const Node& l = nodes_[n.left];
  const Node& r = nodes_[n.right];
  const double dl = box_distance_sq(l.bmin, l.bmax, p), dr = box_distance_sq(r.bmin, r.bmax, p);
  if (dl <= dr) {
    if (dl < best) query(n.left, p, best);
    if (dr < best) query(n.right, p, best);
  } else {
    if (dr < best) query(n.right, p, best);
    if (dl < best) query(n.left, p, best);
  }
}

double TriangleTree::distance_sq(const double* p) const noexcept {
  double best = std::numeric_limits<double>::infinity();
  if (!nodes_.empty()) query(0, p, best);
  return best;
}

}  // namespace rdesk::detail
