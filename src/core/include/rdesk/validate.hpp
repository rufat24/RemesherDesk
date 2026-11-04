// Mesh validation, topology analysis and welding (docs/architecture.md §2).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rdesk/mesh.hpp"
#include "rdesk/result.hpp"

namespace rdesk {

inline constexpr std::size_t kMaxReported = 100;

// A count plus the first kMaxReported offending elements (so the GUI can highlight them).
template <class T>
struct Reported {
  std::size_t count = 0;
  std::vector<T> first;
  void add(T v) {
    ++count;
    if (first.size() < kMaxReported) first.push_back(std::move(v));
  }
};

using EdgeKey = std::array<std::uint32_t, 2>;  // (min vertex, max vertex)

struct ComponentInfo {
  std::size_t vertices = 0;  // referenced vertices
  std::size_t edges = 0;     // unique undirected edges
  std::size_t faces = 0;
  std::size_t boundary_edges = 0;
  std::size_t boundary_loops = 0;
  std::int64_t euler = 0;  // V - E + F
  bool closed = false;     // no boundary edge
  bool manifold = false;   // no non-manifold edge/vertex and consistent orientation
  std::optional<std::int64_t> genus;  // only for closed manifold components
};

struct TopologyInfo {
  std::size_t unique_edges = 0;
  std::size_t boundary_edges = 0;
  // Connected components of the boundary edges. Loops that touch at a non-manifold vertex merge.
  std::size_t boundary_loops = 0;
  std::size_t component_count = 0;  // vertex-connected components
  std::int64_t euler = 0;           // sum over components
  Reported<EdgeKey> nonmanifold_edges;  // more than two incident faces
  Reported<std::uint32_t> nonmanifold_vertices;  // incident faces form more than one fan across 2-face edges
  // Interior edges whose two faces traverse it in the same direction.
  Reported<EdgeKey> orientation_inconsistent_edges;
  std::vector<ComponentInfo> components;  // ordered by smallest vertex index
};

struct ValidationReport {
  std::size_t vertex_count = 0, face_count = 0, corner_count = 0;
  double bbox_diagonal = 0.0;

  // Errors: the mesh cannot be used. Topology is only analysed when there are none of the first three.
  std::vector<std::string> format_errors;  // malformed CSR / attribute sizes
  Reported<std::uint32_t> index_out_of_range_faces;
  Reported<std::uint32_t> faces_too_small;  // fewer than 3 corners
  Reported<std::uint32_t> nonfinite_vertices;

  // Warnings.
  Reported<std::uint32_t> repeated_vertex_faces;
  Reported<std::uint32_t> zero_area_faces;  // area <= 1e-12 * bbox_diagonal^2
  std::size_t unreferenced_vertices = 0;

  bool topology_analyzed = false;
  TopologyInfo topology;  // `components` holds at most kMaxReported entries; see component_count

  bool has_errors() const noexcept {
    return !format_errors.empty() || index_out_of_range_faces.count || faces_too_small.count ||
           nonfinite_vertices.count;
  }
  // Closed, edge- and vertex-manifold, consistently oriented, at least one face.
  bool is_closed_manifold() const noexcept {
    return topology_analyzed && face_count > 0 && topology.boundary_edges == 0 &&
           topology.nonmanifold_edges.count == 0 && topology.nonmanifold_vertices.count == 0 &&
           topology.orientation_inconsistent_edges.count == 0;
  }
};

// Structural problems with the container arrays themselves (offsets, sizes). Empty = well formed.
std::vector<std::string> format_errors(const PolyMesh& m);

ValidationReport validate(const PolyMesh& m);

// Full topology. Fails with InputInvalid unless the CSR is well formed and every index is in range.
Result<TopologyInfo> analyze_topology(const PolyMesh& m);

// Convenience wrappers around analyze_topology.
Result<std::int64_t> euler_characteristic(const PolyMesh& m);
Result<std::size_t> boundary_loops(const PolyMesh& m);
Result<std::size_t> components(const PolyMesh& m);

// Merge vertices whose positions are equal (eps == 0: bitwise, with -0 == +0) or within `eps`
// (Euclidean, greedy: each vertex joins the lowest-index representative within eps).
// The representative's attributes win. Representatives keep their relative order. Faces are
// remapped but never removed, so a weld can create repeated vertices inside a face.
// Non-finite vertices are never merged for eps > 0. ParamInvalid for negative / NaN eps or an eps
// too small for the coordinate range.
Result<PolyMesh> weld_by_position(const PolyMesh& m, double eps = 0.0);

}  // namespace rdesk
