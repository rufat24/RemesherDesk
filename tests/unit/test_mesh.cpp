#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "doctest/doctest.h"
#include "fixtures.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/mesh.hpp"
#include "rdesk/result.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;
using namespace fixtures;

TEST_CASE("Result: value and error paths") {
  Result<int> ok = 7;
  CHECK(ok.ok());
  CHECK(ok.value() == 7);

  Result<int> bad = make_error(Errc::Parse, "nope", "{\"line\":3}");
  CHECK_FALSE(bad.ok());
  CHECK(bad.error().code == Errc::Parse);
  CHECK(bad.error().message == "nope");
  CHECK(bad.error().detail_json == "{\"line\":3}");
  CHECK(std::string(to_string(bad.error().code)) == "parse");

  Result<void> v;
  CHECK(v.ok());
  Result<void> ve = make_error(Errc::Cancelled, "stop");
  CHECK_FALSE(ve.ok());
  CHECK(ve.error().code == Errc::Cancelled);

  Result<std::string> s = std::string("moved");
  const std::string taken = std::move(s).value();
  CHECK(taken == "moved");
}

TEST_CASE("cube: counts, bounding box and volume") {
  const PolyMesh c = make_cube();
  CHECK(c.vertex_count() == 8);
  CHECK(c.face_count() == 6);
  CHECK(c.corner_count() == 24);
  CHECK(c.face_size(0) == 4);
  CHECK(signed_volume(c) == doctest::Approx(1.0));
  const BoundingBox bb = bounding_box(c);
  REQUIRE(bb.valid);
  CHECK(bb.diagonal() == doctest::Approx(std::sqrt(3.0)));
  CHECK(signed_volume(triangulate(c)) == doctest::Approx(1.0));
}

TEST_CASE("bounding box ignores non-finite vertices") {
  PolyMesh m = make_cube();
  m.positions[0] = std::numeric_limits<float>::quiet_NaN();
  const BoundingBox bb = bounding_box(m);
  CHECK(bb.valid);
  CHECK(bounding_box(PolyMesh{}).valid == false);
}

TEST_CASE("convention: Unity conversion keeps outward volume positive") {
  PolyMesh m = make_cube(2, 0, 0);
  convert_in_place(m, Convention::UnityLeftHandedYUpCW);
  CHECK(m.convention == Convention::UnityLeftHandedYUpCW);
  CHECK(signed_volume(m) == doctest::Approx(1.0));
  // X was negated: the cube [2,3] became [-3,-2].
  const BoundingBox bb = bounding_box(m);
  CHECK(bb.min[0] == doctest::Approx(-3.0));
  CHECK(bb.max[0] == doctest::Approx(-2.0));
}

TEST_CASE("convention: half conversions give negative volume") {
  const PolyMesh rh = make_cube();

  PolyMesh negate_only = rh;
  for (std::size_t i = 0; i < negate_only.positions.size(); i += 3) negate_only.positions[i] *= -1.0f;
  CHECK(signed_volume(negate_only) < 0.0);

  PolyMesh reverse_only = rh;
  for (std::size_t f = 0; f < reverse_only.face_count(); ++f) {
    const auto b = reverse_only.face_offsets[f];
    std::reverse(reverse_only.corners.begin() + b + 1, reverse_only.corners.begin() + reverse_only.face_offsets[f + 1]);
  }
  CHECK(signed_volume(reverse_only) < 0.0);
}

TEST_CASE("convention: winding reversal keeps corner 0") {
  PolyMesh m = make_mesh({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0.5f, 2, 0}, {{0, 1, 2, 3}, {3, 2, 4}});
  convert_in_place(m, Convention::UnityLeftHandedYUpCW);
  CHECK(m.corners == std::vector<std::uint32_t>{0, 3, 2, 1, 3, 4, 2});
  CHECK(m.face_offsets == std::vector<std::uint32_t>{0, 4, 7});
}

TEST_CASE("convention: double conversion is bit-identical (incl. attributes)") {
  PolyMesh m = make_chiral();
  const std::size_t nv = m.vertex_count();
  m.attrs.normals.assign(nv * 3, 0.0f);
  m.attrs.tangents.assign(nv * 4, 0.0f);
  for (std::size_t i = 0; i < nv * 3; ++i) m.attrs.normals[i] = 0.25f * static_cast<float>(i) - 1.0f;
  for (std::size_t i = 0; i < nv * 4; ++i) m.attrs.tangents[i] = 0.5f * static_cast<float>(i) - 3.0f;
  m.attrs.normals[0] = 0.0f;
  m.positions[0] = 0.0f;  // +0 survives the round trip

  const PolyMesh orig = m;
  PolyMesh twice = to_convention(to_convention(m, Convention::UnityLeftHandedYUpCW), Convention::RightHandedYUpCCW);
  CHECK(twice.convention == Convention::RightHandedYUpCCW);
  CHECK(bit_equal(twice.positions, orig.positions));
  CHECK(bit_equal(twice.attrs.normals, orig.attrs.normals));
  CHECK(bit_equal(twice.attrs.tangents, orig.attrs.tangents));
  CHECK(twice.corners == orig.corners);
  CHECK(twice.face_offsets == orig.face_offsets);
}

TEST_CASE("convention: converting to the current convention is a no-op") {
  const PolyMesh m = make_chiral();
  const PolyMesh same = to_convention(m, Convention::RightHandedYUpCCW);
  CHECK(bit_equal(same.positions, m.positions));
  CHECK(same.corners == m.corners);
}

TEST_CASE("convention: normals and tangents are mirrored, tangent.w flips") {
  PolyMesh m = make_chiral();
  const std::size_t nv = m.vertex_count();
  m.attrs.normals.assign(nv * 3, 1.0f);
  m.attrs.tangents.assign(nv * 4, 2.0f);
  m.attrs.colors.assign(nv * 4, 0.5f);
  convert_in_place(m, Convention::UnityLeftHandedYUpCW);
  CHECK(m.attrs.normals[0] == -1.0f);
  CHECK(m.attrs.normals[1] == 1.0f);
  CHECK(m.attrs.tangents[0] == -2.0f);
  CHECK(m.attrs.tangents[1] == 2.0f);
  CHECK(m.attrs.tangents[3] == -2.0f);
  CHECK(m.attrs.colors[0] == 0.5f);  // colours are not geometric
}

TEST_CASE("convention: chiral fixture is actually mirrored") {
  const PolyMesh rh = make_chiral();
  // The fixture is a closed tetra pair with outward faces.
  REQUIRE(signed_volume(rh) > 0.0);

  const PolyMesh lh = to_convention(rh, Convention::UnityLeftHandedYUpCW);
  REQUIRE(lh.positions.size() == rh.positions.size());
  bool any_moved = false;
  for (std::size_t i = 0; i < rh.positions.size(); i += 3) {
    CHECK(lh.positions[i] == -rh.positions[i]);
    CHECK(lh.positions[i + 1] == rh.positions[i + 1]);
    CHECK(lh.positions[i + 2] == rh.positions[i + 2]);
    if (lh.positions[i] != rh.positions[i]) any_moved = true;
  }
  CHECK(any_moved);                              // not its own mirror image
  CHECK(signed_volume(lh) > 0.0);               // still outward
  CHECK(signed_volume(lh) == doctest::Approx(signed_volume(rh)));
}

TEST_CASE("weld: exact weld merges equal positions") {
  const PolyMesh two = append(make_cube(), make_cube(1, 0, 0));  // 4 positions coincide
  REQUIRE(two.vertex_count() == 16);
  auto w = weld_by_position(two);
  REQUIRE(w.ok());
  CHECK(w.value().vertex_count() == 12);
  CHECK(w.value().face_count() == 12);
  CHECK(w.value().corners.size() == 48);
  for (const auto c : w.value().corners) CHECK(c < 12);
}

TEST_CASE("weld: -0 and +0 are the same position") {
  const PolyMesh m = make_mesh({0.0f, 0.0f, 0.0f, -0.0f, 0.0f, -0.0f, 1, 1, 1}, {{0, 1, 2}});
  auto w = weld_by_position(m);
  REQUIRE(w.ok());
  CHECK(w.value().vertex_count() == 2);
  CHECK(w.value().corners == std::vector<std::uint32_t>{0, 0, 1});
}

TEST_CASE("weld: representative (lowest index) attributes win and order is preserved") {
  PolyMesh m = make_mesh({0, 0, 0, 5, 5, 5, 0, 0, 0}, {{0, 1, 2}});
  m.attrs.normals = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  m.attrs.uv[0] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
  m.attrs.uv_dim[0] = 2;
  auto w = weld_by_position(m);
  REQUIRE(w.ok());
  const PolyMesh& o = w.value();
  CHECK(o.vertex_count() == 2);
  CHECK(o.corners == std::vector<std::uint32_t>{0, 1, 0});  // faces keep their repeated vertex
  CHECK(o.attrs.normals == std::vector<float>{1, 0, 0, 0, 1, 0});
  CHECK(o.attrs.uv[0] == std::vector<float>{0.1f, 0.2f, 0.3f, 0.4f});
}

TEST_CASE("weld: epsilon weld is greedy, lowest index wins, and validates eps") {
  PolyMesh m = make_mesh({0, 0, 0, 0.0004f, 0, 0, 0.0008f, 0, 0, 10, 0, 0}, {{0, 1, 3}, {2, 3, 1}});
  auto w = weld_by_position(m, 0.0005);
  REQUIRE(w.ok());
  // v1 joins v0 (0.0004 <= eps); v2 is 0.0008 from v0 -> too far; it stays its own representative.
  CHECK(w.value().vertex_count() == 3);
  CHECK(w.value().corners == std::vector<std::uint32_t>{0, 0, 2, 1, 2, 0});
  CHECK(w.value().positions[3 * 1] == 0.0008f);

  CHECK(weld_by_position(m, -1.0).error().code == Errc::ParamInvalid);
  CHECK(weld_by_position(m, std::numeric_limits<double>::quiet_NaN()).error().code == Errc::ParamInvalid);
  CHECK(weld_by_position(m, 1e-30).error().code == Errc::ParamInvalid);  // grid would overflow at x = 10
}

TEST_CASE("weld: non-finite vertices are never merged by epsilon welds") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  PolyMesh m = make_mesh({nan, 0, 0, nan, 0, 0, 1, 1, 1}, {{0, 1, 2}});
  auto w = weld_by_position(m, 0.1);
  REQUIRE(w.ok());
  CHECK(w.value().vertex_count() == 3);
}

TEST_CASE("weld: rejects malformed input") {
  PolyMesh m = make_cube();
  m.corners[0] = 99;
  CHECK(weld_by_position(m).error().code == Errc::InputInvalid);
}
