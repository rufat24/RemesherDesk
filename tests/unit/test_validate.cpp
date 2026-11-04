#include <algorithm>
#include <cmath>
#include <limits>

#include "doctest/doctest.h"
#include "fixtures.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/mesh.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;
using namespace fixtures;

TEST_CASE("validate: cube is a valid closed manifold of genus 0") {
  const ValidationReport r = validate(make_cube());
  CHECK_FALSE(r.has_errors());
  CHECK(r.format_errors.empty());
  REQUIRE(r.topology_analyzed);
  CHECK(r.is_closed_manifold());
  CHECK(r.topology.euler == 2);
  CHECK(r.topology.unique_edges == 12);
  CHECK(r.topology.boundary_edges == 0);
  CHECK(r.topology.boundary_loops == 0);
  CHECK(r.topology.component_count == 1);
  REQUIRE(r.topology.components.size() == 1);
  CHECK(r.topology.components[0].genus == std::optional<std::int64_t>(0));
  CHECK(r.zero_area_faces.count == 0);
  CHECK(r.repeated_vertex_faces.count == 0);
  CHECK(r.unreferenced_vertices == 0);
  CHECK(r.bbox_diagonal == doctest::Approx(std::sqrt(3.0)));
}

TEST_CASE("validate: the Unity-convention cube is just as valid") {
  const ValidationReport r = validate(to_convention(make_cube(), Convention::UnityLeftHandedYUpCW));
  CHECK(r.is_closed_manifold());
}

TEST_CASE("validate: triangulated cube, euler 2") {
  const ValidationReport r = validate(triangulate(make_cube()));
  CHECK(r.is_closed_manifold());
  CHECK(r.topology.euler == 2);
  CHECK(r.topology.unique_edges == 18);
}

TEST_CASE("topology: torus has genus 1") {
  const ValidationReport r = validate(make_torus());
  CHECK(r.is_closed_manifold());
  CHECK(r.topology.euler == 0);
  REQUIRE(r.topology.components.size() == 1);
  CHECK(r.topology.components[0].genus == std::optional<std::int64_t>(1));
  CHECK(signed_volume(make_torus()) > 0.0);  // fixture sanity: faces point outward
}

TEST_CASE("topology: two disjoint cubes") {
  const PolyMesh m = append(make_cube(), make_cube(5, 0, 0));
  const ValidationReport r = validate(m);
  CHECK(r.topology.component_count == 2);
  CHECK(r.topology.euler == 4);
  CHECK(r.is_closed_manifold());
  CHECK(components(m).value() == 2);
  CHECK(euler_characteristic(m).value() == 4);
  CHECK(boundary_loops(m).value() == 0);
}

TEST_CASE("topology: an open quad has one boundary loop and no genus") {
  const PolyMesh m = make_mesh({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}, {{0, 1, 2, 3}});
  const ValidationReport r = validate(m);
  CHECK_FALSE(r.has_errors());
  CHECK(r.topology.boundary_edges == 4);
  CHECK(r.topology.boundary_loops == 1);
  CHECK(r.topology.euler == 1);
  CHECK_FALSE(r.is_closed_manifold());
  REQUIRE(r.topology.components.size() == 1);
  CHECK_FALSE(r.topology.components[0].closed);
  CHECK(r.topology.components[0].manifold);
  CHECK_FALSE(r.topology.components[0].genus.has_value());
}

TEST_CASE("topology: a tube (cube without top and bottom) has two boundary loops") {
  PolyMesh m = make_cube();
  // Remove the top and bottom faces -> a tube with two boundary loops.
  m.face_offsets = {0, 4, 8, 12, 16};
  m.corners = {0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5};
  const ValidationReport r = validate(m);
  CHECK(r.topology.boundary_edges == 8);
  CHECK(r.topology.boundary_loops == 2);
  CHECK(r.topology.euler == 0);
}

TEST_CASE("topology: two cubes sharing an edge -> one non-manifold edge") {
  const PolyMesh raw = append(make_cube(), make_cube(1, 1, 0));
  const PolyMesh m = weld_by_position(raw).value();
  REQUIRE(m.vertex_count() == 14);
  const ValidationReport r = validate(m);
  CHECK_FALSE(r.has_errors());
  CHECK(r.topology.nonmanifold_edges.count == 1);
  REQUIRE(r.topology.nonmanifold_edges.first.size() == 1);
  CHECK(r.topology.nonmanifold_edges.first[0][0] < r.topology.nonmanifold_edges.first[0][1]);
  CHECK(r.topology.nonmanifold_vertices.count == 2);  // the two end points of the shared edge
  CHECK(r.topology.component_count == 1);
  CHECK(r.topology.euler == 3);  // V=14, E=23, F=12
  CHECK_FALSE(r.is_closed_manifold());
  REQUIRE(r.topology.components.size() == 1);
  CHECK_FALSE(r.topology.components[0].manifold);
  CHECK_FALSE(r.topology.components[0].genus.has_value());
}

TEST_CASE("topology: bow-tie vertex is detected") {
  const PolyMesh m = make_mesh({0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0}, {{0, 1, 2}, {0, 3, 4}});
  const ValidationReport r = validate(m);
  CHECK(r.topology.nonmanifold_edges.count == 0);
  REQUIRE(r.topology.nonmanifold_vertices.count == 1);
  CHECK(r.topology.nonmanifold_vertices.first[0] == 0);
  CHECK(r.topology.component_count == 1);  // vertex-connected
  CHECK(r.topology.boundary_edges == 6);
  CHECK_FALSE(r.is_closed_manifold());
}

TEST_CASE("topology: a flipped face is an orientation inconsistency") {
  PolyMesh m = make_cube();
  std::reverse(m.corners.begin() + 1, m.corners.begin() + 4);  // face 0: [0,3,2,1] -> [0,1,2,3]
  const ValidationReport r = validate(m);
  CHECK(r.topology.nonmanifold_edges.count == 0);
  CHECK(r.topology.nonmanifold_vertices.count == 0);
  CHECK(r.topology.orientation_inconsistent_edges.count == 4);
  CHECK(r.topology.boundary_edges == 0);
  CHECK_FALSE(r.is_closed_manifold());
  REQUIRE(r.topology.components.size() == 1);
  CHECK_FALSE(r.topology.components[0].genus.has_value());
}

TEST_CASE("validate: structural errors") {
  SUBCASE("index out of range") {
    PolyMesh m = make_cube();
    m.corners[5] = 8;
    const ValidationReport r = validate(m);
    CHECK(r.has_errors());
    CHECK(r.index_out_of_range_faces.count == 1);
    CHECK(r.index_out_of_range_faces.first[0] == 1);
    CHECK_FALSE(r.topology_analyzed);
    CHECK(analyze_topology(m).error().code == Errc::InputInvalid);
  }
  SUBCASE("face with fewer than 3 corners") {
    PolyMesh m = make_mesh({0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0, 1}, {0, 1, 2}});
    const ValidationReport r = validate(m);
    CHECK(r.has_errors());
    CHECK(r.faces_too_small.count == 1);
    CHECK(r.faces_too_small.first[0] == 0);
  }
  SUBCASE("non-finite position") {
    PolyMesh m = make_cube();
    m.positions[4] = std::numeric_limits<float>::infinity();
    const ValidationReport r = validate(m);
    CHECK(r.has_errors());
    REQUIRE(r.nonfinite_vertices.count == 1);
    CHECK(r.nonfinite_vertices.first[0] == 1);
  }
  SUBCASE("malformed containers are reported, not crashed on") {
    PolyMesh bad_total = make_cube();
    bad_total.face_offsets.back() = 25;
    CHECK(validate(bad_total).has_errors());
    CHECK_FALSE(validate(bad_total).format_errors.empty());

    PolyMesh bad_first = make_cube();
    bad_first.face_offsets.front() = 1;
    CHECK_FALSE(validate(bad_first).format_errors.empty());

    PolyMesh not_monotonic = make_cube();
    not_monotonic.face_offsets[2] = 3;
    CHECK_FALSE(validate(not_monotonic).format_errors.empty());

    PolyMesh bad_pos = make_cube();
    bad_pos.positions.pop_back();
    CHECK_FALSE(validate(bad_pos).format_errors.empty());

    PolyMesh bad_group = make_cube();
    bad_group.face_group = {0, 1};
    CHECK_FALSE(validate(bad_group).format_errors.empty());

    PolyMesh bad_normals = make_cube();
    bad_normals.attrs.normals.assign(5, 0.0f);
    CHECK_FALSE(validate(bad_normals).format_errors.empty());

    PolyMesh bad_uv = make_cube();
    bad_uv.attrs.uv[0].assign(16, 0.0f);  // data but uv_dim == 0
    CHECK_FALSE(validate(bad_uv).format_errors.empty());

    CHECK_FALSE(validate(PolyMesh{}).has_errors());  // empty mesh is well formed
    CHECK(analyze_topology(bad_total).error().code == Errc::InputInvalid);
  }
}

TEST_CASE("validate: warnings") {
  SUBCASE("repeated vertex inside a face") {
    PolyMesh m = make_cube();
    m.corners.insert(m.corners.end(), {0, 1, 1, 2});
    m.face_offsets.push_back(static_cast<std::uint32_t>(m.corners.size()));
    const ValidationReport r = validate(m);
    CHECK_FALSE(r.has_errors());
    CHECK(r.repeated_vertex_faces.count == 1);
    CHECK(r.repeated_vertex_faces.first[0] == 6);
  }
  SUBCASE("zero-area face and unreferenced vertices") {
    PolyMesh m = make_cube();
    m.positions.insert(m.positions.end(), {0.5f, 0, 0});  // vertex 8, collinear with 0 and 1
    m.corners.insert(m.corners.end(), {0, 1, 8});
    m.face_offsets.push_back(static_cast<std::uint32_t>(m.corners.size()));
    m.positions.insert(m.positions.end(), {9, 9, 9});  // vertex 9, unreferenced
    const ValidationReport r = validate(m);
    CHECK_FALSE(r.has_errors());
    REQUIRE(r.zero_area_faces.count == 1);
    CHECK(r.zero_area_faces.first[0] == 6);
    CHECK(r.unreferenced_vertices == 1);
  }
  SUBCASE("a healthy sliver is not flagged") {
    PolyMesh m = make_mesh({0, 0, 0, 1, 0, 0, 0.5f, 1e-3f, 0}, {{0, 1, 2}});
    CHECK(validate(m).zero_area_faces.count == 0);
  }
}

TEST_CASE("validate: reports are capped at kMaxReported entries but count everything") {
  PolyMesh m;
  const std::size_t n = 250;
  m.positions.assign(n * 3, std::numeric_limits<float>::quiet_NaN());
  m.face_offsets = {0};
  const ValidationReport r = validate(m);
  CHECK(r.nonfinite_vertices.count == n);
  CHECK(r.nonfinite_vertices.first.size() == kMaxReported);
  CHECK(r.nonfinite_vertices.first.front() == 0);
  CHECK(r.nonfinite_vertices.first.back() == kMaxReported - 1);
  CHECK(r.unreferenced_vertices == n);
}

TEST_CASE("validate: the component list is capped but component_count is not") {
  PolyMesh m = make_cube();
  for (int i = 1; i < 120; ++i) m = append(m, make_cube(static_cast<float>(3 * i), 0, 0));
  const ValidationReport r = validate(m);
  CHECK(r.topology.component_count == 120);
  CHECK(r.topology.components.size() == kMaxReported);
  CHECK(r.topology.euler == 240);
  CHECK(r.is_closed_manifold());
}
