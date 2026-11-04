// Internal: AABB tree over the triangles of a polygon mesh, for exact point-to-surface distance.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "rdesk/mesh.hpp"

namespace rdesk::detail {

// Closest-point distance on a triangle (Ericson, "Real-Time Collision Detection" 5.1.5), in double.
double point_triangle_distance_sq(const double* p, const double* a, const double* b, const double* c) noexcept;

class TriangleTree {
 public:
  // Triangles: a triangle face as is; a quad as FOUR triangles (both diagonals), so a non-planar quad is
  // measured against the nearer of its two possible folds; an n-gon (n > 4) as a fan from corner 0.
  // The minimum over both diagonals underestimates the distance to a non-planar quad by at most its
  // non-planarity (reported by compute_stats). Faces with fewer than 3 corners are ignored.
  // Precondition: indices are in range (checked by the caller).
  explicit TriangleTree(const PolyMesh& mesh);

  bool empty() const noexcept { return tris_.empty(); }
  // Squared distance from p to the nearest triangle; +infinity when empty.
  double distance_sq(const double* p) const noexcept;

 private:
  struct Node {
    double bmin[3], bmax[3];
    std::uint32_t left = 0, right = 0;  // internal: children
    std::uint32_t first = 0, count = 0;  // leaf when count > 0
  };
  std::uint32_t build(std::uint32_t lo, std::uint32_t hi);
  void query(std::uint32_t node, const double* p, double& best) const noexcept;
  void tri_vertex(std::uint32_t tri, int k, double* out) const noexcept;

  const PolyMesh* mesh_;
  std::vector<std::array<std::uint32_t, 3>> tris_;
  std::vector<Node> nodes_;
};

}  // namespace rdesk::detail
