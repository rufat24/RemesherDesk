// Property tests (docs/testing.md): 200 random meshes, generated from subdivided platonic solids plus a random
// perturbation. Every failure message carries the case seed, so a failure can be replayed with
// RDESK_PROPERTY_SEED=<seed> RDESK_PROPERTY_CASES=1. Set RDESK_PROPERTY_CASES=5000 for a longer local run.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <map>

#include "doctest/doctest.h"
#include "pcg32.hpp"
#include "rdesk/metrics.hpp"
#include "rdesk/validate.hpp"
#include "fixtures.hpp"

using namespace rdesk;

namespace {

PolyMesh platonic(int which) {
  const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
  switch (which) {
    case 0:  // tetrahedron
      return fixtures::make_mesh({1, 1, 1, 1, -1, -1, -1, 1, -1, -1, -1, 1}, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
    case 1:  // octahedron
      return fixtures::make_mesh({1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1},
                                 {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}});
    default:  // icosahedron
      return fixtures::make_mesh(
          {-1, t, 0, 1, t, 0, -1, -t, 0, 1, -t, 0, 0, -1, t, 0, 1, t, 0, -1, -t, 0, 1, -t, t, 0, -1, t, 0, 1, -t, 0, -1, -t, 0, 1},
          {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
           {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}});
  }
}

// 1 -> 4 triangle split at the edge midpoints.
PolyMesh subdivide(const PolyMesh& m) {
  PolyMesh out;
  out.positions = m.positions;
  out.face_offsets.push_back(0);
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
  auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
    const auto key = std::minmax(a, b);
    auto it = mid.find({key.first, key.second});
    if (it != mid.end()) return it->second;
    const auto id = static_cast<std::uint32_t>(out.positions.size() / 3);
    for (std::size_t k = 0; k < 3; ++k) out.positions.push_back(0.5f * (m.positions[3 * a + k] + m.positions[3 * b + k]));
    mid.emplace(std::make_pair(key.first, key.second), id);
    return id;
  };
  for (std::size_t f = 0; f < m.face_count(); ++f) {
    const std::uint32_t* c = &m.corners[m.face_offsets[f]];
    const std::uint32_t ab = midpoint(c[0], c[1]), bc = midpoint(c[1], c[2]), ca = midpoint(c[2], c[0]);
    for (const auto& t : {std::array<std::uint32_t, 3>{c[0], ab, ca}, {c[1], bc, ab}, {c[2], ca, bc}, {ab, bc, ca}}) {
      out.corners.insert(out.corners.end(), t.begin(), t.end());
      out.face_offsets.push_back(static_cast<std::uint32_t>(out.corners.size()));
    }
  }
  return out;
}

std::uint64_t base_seed() {
  const auto v = fixtures::env_value("RDESK_PROPERTY_SEED");
  return v ? std::strtoull(v->c_str(), nullptr, 10) : 1000;
}
int case_count() {
  const auto v = fixtures::env_value("RDESK_PROPERTY_CASES");
  return v ? std::atoi(v->c_str()) : 200;
}

}  // namespace

TEST_CASE("properties of random subdivided and perturbed platonic solids") {
  DistanceOptions opts;
  opts.min_samples = 1500;
  opts.samples_per_face = 1;

  for (int i = 0; i < case_count(); ++i) {
    const std::uint64_t seed = base_seed() + static_cast<std::uint64_t>(i);
    INFO("replay with RDESK_PROPERTY_SEED=" << seed << " RDESK_PROPERTY_CASES=1");
    detail::Pcg32 rng(seed);
    PolyMesh a = platonic(static_cast<int>(rng.next() % 3));
    const unsigned levels = rng.next() % 3;
    for (unsigned l = 0; l < levels; ++l) a = subdivide(a);
    const float scale = 0.25f + static_cast<float>(rng.next_double()) * 8.0f;
    const float shift = static_cast<float>(rng.next_double()) * 20.0f - 10.0f;
    for (float& x : a.positions) x = x * scale + shift;
    // Random perturbation of the base shape, so the two meshes are different surfaces.
    const double d = bounding_box(a).diagonal();
    const double delta = (0.002 + 0.03 * rng.next_double()) * d;
    PolyMesh b = a;
    double worst = 0;
    for (float& x : b.positions) {
      const double off = (rng.next_double() * 2.0 - 1.0) * delta;
      worst = std::max(worst, std::fabs(off));
      x += static_cast<float>(off);
    }
    worst += 1e-6 * d;  // float rounding of the perturbation

    // 1. A subdivided platonic solid is a closed manifold sphere.
    const ValidationReport va = validate(a);
    REQUIRE_FALSE(va.has_errors());
    REQUIRE(va.is_closed_manifold());
    REQUIRE(va.topology.euler == 2);
    REQUIRE(va.topology.components[0].genus == std::optional<std::int64_t>(0));

    // 2. Self-distance is zero.
    auto self = surface_distance(a, a, opts);
    REQUIRE(self.ok());
    REQUIRE(self.value().hausdorff() < 1e-9 * d);

    // 3. The one-sided distances swap exactly when the arguments swap.
    auto ab = surface_distance(a, b, opts), ba = surface_distance(b, a, opts);
    REQUIRE(ab.ok());
    REQUIRE(ba.ok());
    REQUIRE(ab.value().a_to_b.max == ba.value().b_to_a.max);
    REQUIRE(ab.value().b_to_a.mean == ba.value().a_to_b.mean);
    REQUIRE(ab.value().hausdorff() == ba.value().hausdorff());

    // 4. Moving every vertex by at most `worst` per axis moves the surface by at most sqrt(3) * worst.
    REQUIRE(ab.value().hausdorff() <= std::sqrt(3.0) * worst * (1 + 1e-9));
    REQUIRE(ab.value().chamfer() <= ab.value().hausdorff() + 1e-12);
    REQUIRE(ab.value().rms() >= ab.value().chamfer() - 1e-12);

    // 5. Scaling both meshes by 2 (exact in floating point) scales every distance by 2.
    PolyMesh a2 = a, b2 = b;
    for (float& x : a2.positions) x *= 2.0f;
    for (float& x : b2.positions) x *= 2.0f;
    auto big = surface_distance(a2, b2, opts);
    REQUIRE(big.ok());
    REQUIRE(big.value().hausdorff() == doctest::Approx(2.0 * ab.value().hausdorff()).epsilon(1e-9));
    REQUIRE(big.value().chamfer() == doctest::Approx(2.0 * ab.value().chamfer()).epsilon(1e-9));

    // 6. Statistics are consistent with the topology report.
    auto st = compute_stats(a);
    REQUIRE(st.ok());
    REQUIRE(st.value().unique_edges == va.topology.unique_edges);
    REQUIRE(st.value().triangles == a.face_count());
  }
}
