// Mesh statistics, surface distances and parity comparison (docs/testing.md §3).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rdesk/mesh.hpp"
#include "rdesk/result.hpp"

namespace rdesk {

struct Range {
  double min = 0, mean = 0, max = 0;
};

struct MeshStats {
  std::size_t vertices = 0;  // all vertices
  std::size_t referenced_vertices = 0;
  std::size_t faces = 0;
  std::size_t triangles = 0, quads = 0, ngons = 0;  // faces with 3 / 4 / more corners (smaller faces are ignored)
  double quad_ratio = 0;       // quads / faces
  std::size_t unique_edges = 0;
  // valence_histogram[k] = number of vertices with k incident edges, the last bucket collects k >= 16.
  std::array<std::size_t, 17> valence_histogram{};
  // Interior vertices with valence != 4 plus boundary vertices with valence != 3 (the quad-mesh convention).
  std::size_t irregular_vertices = 0;
  Range edge_length;           // over unique edges
  Range quad_aspect;           // longest / shortest edge of each quad (>= 1)
  double quad_angle_dev_mean_deg = 0, quad_angle_dev_max_deg = 0;  // |interior angle - 90 deg|
  double max_quad_nonplanarity = 0;  // largest distance of corner 3 from the plane of corners 0,1,2
  double surface_area = 0;     // fan triangulation from corner 0
  BoundingBox bbox;
};

// Requires a well-formed container and in-range indices; otherwise Errc::InputInvalid.
Result<MeshStats> compute_stats(const PolyMesh& m);

struct DistanceOptions {
  std::size_t min_samples = 100000;     // N = max(min_samples, samples_per_face * faces) area samples per surface
  std::size_t samples_per_face = 10;
  std::uint64_t seed = 0x52444553;      // fixed: results are reproducible
  bool include_vertices = true;         // also measure every referenced vertex (affects max only)
};

struct OneSidedDistance {
  double max = 0;   // over area samples and vertices
  double mean = 0;  // over area samples
  double rms = 0;   // over area samples
  std::size_t samples = 0;
};

struct DistanceReport {
  OneSidedDistance a_to_b, b_to_a;
  double hausdorff() const noexcept { return a_to_b.max > b_to_a.max ? a_to_b.max : b_to_a.max; }
  double chamfer() const noexcept { return 0.5 * (a_to_b.mean + b_to_a.mean); }
  double rms() const noexcept;
  double max_quad_nonplanarity = 0;  // bound on the bias of the "both diagonals" quad treatment
};

// Sampled distances between two surfaces (area-uniform samples, in-house PCG32, exact point-triangle distance
// through an AABB tree). Errc::InputInvalid for malformed meshes or meshes without a usable face.
Result<DistanceReport> surface_distance(const PolyMesh& a, const PolyMesh& b, const DistanceOptions& opts = {});

// Area-uniform sample points of `m` (fan triangulation), x y z per point. Deterministic for a given seed and
// identical on every compiler: only integer arithmetic, IEEE +,-,*,/ and sqrt are involved.
std::vector<double> sample_surface(const PolyMesh& m, std::size_t count, std::uint64_t seed);

enum class Tier { T0, T1, T2 };
const char* to_string(Tier t) noexcept;

struct Check {
  std::string name;
  double value = 0;
  double limit = 0;   // the check passes when value <= limit (or value == limit == 0 for equality checks)
  bool pass = false;
  std::string note;
};

struct CompareReport {
  Tier tier = Tier::T2;
  bool pass = false;
  std::vector<Check> checks;
  std::string to_json() const;
};

// `ref` is the reference (for example Unity's output), `test` the output under test.
// T0: identical positions (bitwise) and identical connectivity, same order.
// T1: same V and F, identical connectivity (same order, or after relabelling vertices that match by
//     exact position) and max vertex displacement <= 1e-5 * d (d = reference bbox diagonal).
// T2: Hausdorff <= 0.25 * mean reference edge length and <= 1e-2 * d, face count +-2%, quad ratio +-0.01,
//     irregular vertices within max(10%, 4), genus / components / boundary loops equal, manifold and
//     orientation flags equal.
Result<CompareReport> compare(const PolyMesh& ref, const PolyMesh& test, Tier tier, const DistanceOptions& opts = {});

}  // namespace rdesk
