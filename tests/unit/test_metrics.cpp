#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "doctest/doctest.h"
#include "fixtures.hpp"
#include "pcg32.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/metrics.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;
using namespace fixtures;

namespace {
DistanceOptions fast() {
  DistanceOptions o;
  o.min_samples = 4000;
  o.samples_per_face = 1;
  return o;
}
PolyMesh scaled_about_center(PolyMesh m, float s) {
  for (std::size_t i = 0; i < m.positions.size(); ++i) m.positions[i] = (m.positions[i] - 0.5f) * s + 0.5f;
  return m;
}
std::uint64_t fnv1a(const std::vector<float>& v) {
  std::uint64_t h = 1469598103934665603ull;
  for (const float f : v) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    for (int i = 0; i < 4; ++i) {
      h ^= (b >> (8 * i)) & 0xff;
      h *= 1099511628211ull;
    }
  }
  return h;
}
}  // namespace

TEST_CASE("pcg32: known-answer test (O'Neill's reference, seed 42, stream 54)") {
  detail::Pcg32 r(42, 54);
  const std::uint32_t expect[] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
  for (const std::uint32_t e : expect) CHECK(r.next() == e);
  detail::Pcg32 d(1);
  for (int i = 0; i < 1000; ++i) {
    const double x = d.next_double();
    REQUIRE(x >= 0.0);
    REQUIRE(x < 1.0);
  }
}

TEST_CASE("stats: unit cube") {
  auto r = compute_stats(make_cube());
  REQUIRE(r.ok());
  const MeshStats& s = r.value();
  CHECK(s.vertices == 8);
  CHECK(s.faces == 6);
  CHECK(s.quads == 6);
  CHECK(s.triangles == 0);
  CHECK(s.quad_ratio == 1.0);
  CHECK(s.unique_edges == 12);
  CHECK(s.valence_histogram[3] == 8);
  CHECK(s.irregular_vertices == 8);  // interior valence 3 != 4
  CHECK(s.edge_length.min == doctest::Approx(1.0));
  CHECK(s.edge_length.max == doctest::Approx(1.0));
  CHECK(s.quad_aspect.max == doctest::Approx(1.0));
  CHECK(s.quad_angle_dev_max_deg == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(s.max_quad_nonplanarity == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(s.surface_area == doctest::Approx(6.0));
  CHECK(s.bbox.diagonal() == doctest::Approx(std::sqrt(3.0)));
}

TEST_CASE("stats: triangulated cube and n-gons") {
  auto t = compute_stats(triangulate(make_cube()));
  REQUIRE(t.ok());
  CHECK(t.value().triangles == 12);
  CHECK(t.value().quad_ratio == 0.0);
  CHECK(t.value().unique_edges == 18);
  CHECK(t.value().surface_area == doctest::Approx(6.0));

  const PolyMesh pent = make_mesh({0, 0, 0, 2, 0, 0, 3, 1, 0, 1.5f, 2, 0, -1, 1, 0}, {{0, 1, 2, 3, 4}});
  auto p = compute_stats(pent);
  REQUIRE(p.ok());
  CHECK(p.value().ngons == 1);
  CHECK(p.value().triangles == 0);
}

TEST_CASE("stats: a regular grid has only its four corners irregular") {
  auto r = compute_stats(make_grid(4, 4));
  REQUIRE(r.ok());
  CHECK(r.value().vertices == 25);
  CHECK(r.value().irregular_vertices == 4);   // corners: boundary valence 2
  CHECK(r.value().valence_histogram[4] == 9);  // interior
  CHECK(r.value().valence_histogram[3] == 12); // boundary, non-corner
  CHECK(r.value().valence_histogram[2] == 4);
}

TEST_CASE("stats: quad quality and non-planarity") {
  // A 2 x 1 rectangle with its 4th corner lifted by 0.3.
  const PolyMesh m = make_mesh({0, 0, 0, 2, 0, 0, 2, 1, 0, 0, 1, 0.3f}, {{0, 1, 2, 3}});
  auto r = compute_stats(m);
  REQUIRE(r.ok());
  CHECK(r.value().quad_aspect.max > 1.9);
  CHECK(r.value().max_quad_nonplanarity == doctest::Approx(0.3).epsilon(1e-6));
  CHECK(r.value().quad_angle_dev_max_deg > 0.0);
}

TEST_CASE("stats: rejects malformed meshes") {
  PolyMesh m = make_cube();
  m.corners[0] = 99;
  CHECK(compute_stats(m).error().code == Errc::InputInvalid);
}

TEST_CASE("distance: a mesh against itself is at distance 0") {
  for (const PolyMesh& m : {make_cube(), triangulate(make_cube()), make_torus(), make_grid(3, 2)}) {
    auto r = surface_distance(m, m, fast());
    REQUIRE(r.ok());
    CHECK(r.value().hausdorff() < 1e-12);
    CHECK(r.value().chamfer() < 1e-12);
    CHECK(r.value().rms() < 1e-12);
  }
}

TEST_CASE("distance: cube against the same cube scaled by 1.01") {
  const PolyMesh a = make_cube();
  const PolyMesh b = scaled_about_center(a, 1.01f);
  auto r = surface_distance(a, b);  // default sampling: 1e5 samples per surface plus the vertices
  REQUIRE(r.ok());
  // Original -> scaled: every point is 0.005 from the nearest face of the larger cube.
  CHECK(r.value().a_to_b.max == doctest::Approx(0.005).epsilon(0.05));
  // Scaled -> original: the corners of the larger cube are sqrt(3) * 0.005 from the original's corners.
  CHECK(r.value().b_to_a.max == doctest::Approx(0.005 * std::sqrt(3.0)).epsilon(0.05));
  CHECK(r.value().hausdorff() == doctest::Approx(0.005 * std::sqrt(3.0)).epsilon(0.05));
}

TEST_CASE("distance: two parallel planes are exactly their separation apart") {
  auto r = surface_distance(make_grid(5, 5), make_grid(5, 5, 1.0f, 0.3f), fast());
  REQUIRE(r.ok());
  CHECK(r.value().a_to_b.max == doctest::Approx(0.3).epsilon(1e-6));
  CHECK(r.value().a_to_b.mean == doctest::Approx(0.3).epsilon(1e-6));
  CHECK(r.value().a_to_b.rms == doctest::Approx(0.3).epsilon(1e-6));
  CHECK(r.value().chamfer() == doctest::Approx(0.3).epsilon(1e-6));
}

TEST_CASE("distance: a non-planar quad is measured against the nearer fold; the bias is reported") {
  const PolyMesh flat = make_grid(1, 1);
  PolyMesh bent = flat;
  bent.positions[3 * 2 + 2] = 0.2f;  // lift corner 2
  auto r = surface_distance(bent, flat, fast());
  REQUIRE(r.ok());
  CHECK(r.value().max_quad_nonplanarity == doctest::Approx(0.2).epsilon(1e-6));
  CHECK(r.value().hausdorff() <= static_cast<double>(0.2f) + 1e-9);
}

TEST_CASE("distance: errors") {
  PolyMesh empty;
  empty.face_offsets = {0};
  CHECK(surface_distance(empty, make_cube()).error().code == Errc::InputInvalid);
  CHECK(surface_distance(make_cube(), empty).error().code == Errc::InputInvalid);
  PolyMesh bad = make_cube();
  bad.corners[0] = 50;
  CHECK(surface_distance(bad, make_cube()).error().code == Errc::InputInvalid);
}

TEST_CASE("sampling: deterministic, seed-dependent, area-uniform") {
  const PolyMesh c = make_cube();
  const auto s1 = sample_surface(c, 1000, 7), s2 = sample_surface(c, 1000, 7), s3 = sample_surface(c, 1000, 8);
  CHECK(s1 == s2);
  CHECK(s1 != s3);
  CHECK(s1.size() == 3000);

  // Every sample lies on the surface of the unit cube (at least one coordinate is 0 or 1).
  for (std::size_t i = 0; i < 1000; ++i) {
    bool on_face = false;
    for (int k = 0; k < 3; ++k) {
      const double v = s1[3 * i + static_cast<std::size_t>(k)];
      CHECK(v >= -1e-12);
      CHECK(v <= 1 + 1e-12);
      on_face = on_face || v < 1e-12 || v > 1 - 1e-12;
    }
    CHECK(on_face);
  }
  // Uniform over faces: each of the six faces gets about 1/6 of 60000 samples.
  const auto big = sample_surface(c, 60000, 99);
  std::size_t per_face[6] = {};
  for (std::size_t i = 0; i < 60000; ++i) {
    for (std::size_t k = 0; k < 3; ++k) {
      const double v = big[3 * i + k];
      if (v < 1e-12) ++per_face[2 * k];
      if (v > 1 - 1e-12) ++per_face[2 * k + 1];
    }
  }
  for (const std::size_t n : per_face) CHECK(std::abs(static_cast<double>(n) - 10000.0) < 400.0);
}

TEST_CASE("sampling: the same seed gives the same bytes on every compiler (pinned checksum)") {
  // If this fails on one compiler only, cross-compiler determinism of the samples is broken
  // (docs/testing.md §4): investigate before touching the constant.
  std::vector<float> narrowed;
  for (const double d : sample_surface(make_cube(), 2000, 12345)) narrowed.push_back(static_cast<float>(d));
  CHECK(fnv1a(narrowed) == 10141219514054598709ull);
}

TEST_CASE("compare T0: identical meshes pass, any bit change fails") {
  const PolyMesh m = make_torus();
  auto same = compare(m, m, Tier::T0);
  REQUIRE(same.ok());
  CHECK(same.value().pass);
  PolyMesh t = m;
  t.positions[10] = std::nextafter(t.positions[10], 10.0f);
  auto bit = compare(m, t, Tier::T0);
  REQUIRE(bit.ok());
  CHECK_FALSE(bit.value().pass);
  PolyMesh reordered = m;
  std::swap(reordered.corners[0], reordered.corners[1]);
  CHECK_FALSE(compare(m, reordered, Tier::T0).value().pass);
}

TEST_CASE("compare T1: displacement limit, face order and vertex relabelling") {
  const PolyMesh m = make_torus(8, 6);
  const double d = bounding_box(m).diagonal();

  PolyMesh near = m;
  near.positions[7] += static_cast<float>(1e-6 * d);
  CHECK(compare(m, near, Tier::T1).value().pass);
  PolyMesh far = m;
  far.positions[7] += static_cast<float>(1e-3 * d);
  auto rf = compare(m, far, Tier::T1);
  REQUIRE(rf.ok());
  CHECK_FALSE(rf.value().pass);

  // Same faces in a different order: connectivity equal after canonicalisation.
  PolyMesh shuffled = m;
  shuffled.face_offsets = {0};
  shuffled.corners.clear();
  for (std::size_t k = m.face_count(); k-- > 0;) {
    shuffled.corners.insert(shuffled.corners.end(), m.corners.begin() + m.face_offsets[k], m.corners.begin() + m.face_offsets[k + 1]);
    shuffled.face_offsets.push_back(static_cast<std::uint32_t>(shuffled.corners.size()));
  }
  auto rs = compare(m, shuffled, Tier::T1);
  REQUIRE(rs.ok());
  CHECK(rs.value().pass);

  // Vertices permuted (reverse order), faces relabelled to match.
  PolyMesh perm = m;
  const std::size_t nv = m.vertex_count();
  for (std::size_t v = 0; v < nv; ++v) {
    for (std::size_t k = 0; k < 3; ++k) perm.positions[3 * (nv - 1 - v) + k] = m.positions[3 * v + k];
  }
  for (auto& c : perm.corners) c = static_cast<std::uint32_t>(nv - 1) - c;
  auto rp = compare(m, perm, Tier::T1);
  REQUIRE(rp.ok());
  CHECK(rp.value().pass);

  // Different topology fails.
  CHECK_FALSE(compare(m, triangulate(m), Tier::T1).value().pass);
}

TEST_CASE("compare T2: statistical equivalence") {
  const PolyMesh ref = make_grid(8, 8);
  SUBCASE("tiny noise passes") {
    PolyMesh t = ref;
    for (std::size_t i = 0; i < t.positions.size(); i += 3) t.positions[i] += 1e-4f * static_cast<float>(i % 5);
    auto r = compare(ref, t, Tier::T2, fast());
    REQUIRE(r.ok());
    CHECK_MESSAGE(r.value().pass, r.value().to_json());
  }
  SUBCASE("a shifted surface fails the Hausdorff limit") {
    auto r = compare(ref, make_grid(8, 8, 1.0f, 0.5f), Tier::T2, fast());
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().pass);
  }
  SUBCASE("a triangulation of the same surface fails the quad-ratio limit") {
    auto r = compare(ref, triangulate(ref), Tier::T2, fast());
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().pass);
  }
  SUBCASE("different genus fails") {
    auto r = compare(make_torus(8, 6), make_cube(), Tier::T2, fast());
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().pass);
  }
  SUBCASE("invalid input is an error, not a verdict") {
    PolyMesh bad = ref;
    bad.corners[0] = 999;
    CHECK(compare(ref, bad, Tier::T2).error().code == Errc::InputInvalid);
  }
}

TEST_CASE("compare: JSON report") {
  auto r = compare(make_cube(), make_cube(), Tier::T0);
  REQUIRE(r.ok());
  const std::string j = r.value().to_json();
  CHECK(j.front() == '{');
  CHECK(j.back() == '}');
  CHECK(j.find("\"tier\":\"T0\"") != std::string::npos);
  CHECK(j.find("\"pass\":true") != std::string::npos);
  CHECK(j.find("\"name\":\"position_bits_differing\"") != std::string::npos);

  CompareReport odd;
  odd.checks.push_back({"quote\"and\\slash\nnl", std::numeric_limits<double>::infinity(), 1.0, false, "tab\t"});
  const std::string o = odd.to_json();
  CHECK(o.find("quote\\\"and\\\\slash\\nnl") != std::string::npos);
  CHECK(o.find("\"value\":null") != std::string::npos);
  CHECK(o.find("tab\\t") != std::string::npos);
}

#include "rdesk/sha256.hpp"

TEST_CASE("sha256: FIPS 180-4 test vectors, chunked input and lengths around the block size") {
  CHECK(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(sha256_hex(two_blocks.data(), two_blocks.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  Sha256 million;  // one million 'a', fed in odd-sized pieces
  const std::string chunk(997, 'a');
  std::size_t left = 1000000;
  while (left > 0) {
    const std::size_t n = std::min(left, chunk.size());
    million.update(chunk.data(), n);
    left -= n;
  }
  CHECK(million.hex_digest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  // Splitting the input anywhere gives the same digest as hashing it in one go.
  std::string data(300, '\0');
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 7 + 3);
  const std::string whole = sha256_hex(data.data(), data.size());
  for (std::size_t cut : {1u, 55u, 56u, 63u, 64u, 65u, 119u, 128u, 299u}) {
    Sha256 h;
    h.update(data.data(), cut);
    h.update(data.data() + cut, data.size() - cut);
    CHECK(h.hex_digest() == whole);
  }
}
