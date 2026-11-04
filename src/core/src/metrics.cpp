#include "rdesk/metrics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "geometry.hpp"
#include "rdesk/validate.hpp"

namespace rdesk {

const char* to_string(Tier t) noexcept {
  switch (t) {
    case Tier::T0: return "T0";
    case Tier::T1: return "T1";
    case Tier::T2: return "T2";
  }
  return "T2";
}

namespace {

constexpr double kPi = 3.14159265358979323846;

Result<void> check_indices(const PolyMesh& m, const char* which) {
  if (auto fe = format_errors(m); !fe.empty()) {
    return make_error(Errc::InputInvalid, std::string(which) + ": malformed mesh container: " + fe.front());
  }
  for (const std::uint32_t c : m.corners) {
    if (c >= m.vertex_count()) return make_error(Errc::InputInvalid, std::string(which) + ": corner index out of range");
  }
  return {};
}

struct Edge {
  std::uint32_t lo, hi;
  bool operator<(const Edge& o) const noexcept { return lo != o.lo ? lo < o.lo : hi < o.hi; }
  bool operator==(const Edge& o) const noexcept { return lo == o.lo && hi == o.hi; }
};

const float* P(const PolyMesh& m, std::uint32_t v) noexcept { return &m.positions[3 * std::size_t{v}]; }

double dist(const float* a, const float* b) noexcept {
  const double x = double(a[0]) - b[0], y = double(a[1]) - b[1], z = double(a[2]) - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

Result<MeshStats> compute_stats(const PolyMesh& m) {
  if (auto r = check_indices(m, "mesh"); !r) return r.error();
  MeshStats s;
  s.vertices = m.vertex_count();
  s.faces = m.face_count();
  s.bbox = bounding_box(m);

  std::vector<Edge> edges;
  edges.reserve(m.corners.size());
  std::vector<std::uint8_t> referenced(s.vertices, 0);
  double aspect_sum = 0, dev_sum = 0;
  std::size_t quad_n = 0;
  s.quad_aspect.min = std::numeric_limits<double>::infinity();
  for (std::size_t f = 0; f < s.faces; ++f) {
    const std::uint32_t b = m.face_offsets[f], n = m.face_offsets[f + 1] - b;
    const std::uint32_t* c = &m.corners[b];
    for (std::uint32_t k = 0; k < n; ++k) {
      referenced[c[k]] = 1;
      const std::uint32_t u = c[k], v = c[(k + 1) % n];
      if (u != v) edges.push_back({std::min(u, v), std::max(u, v)});
    }
    if (n == 3) ++s.triangles;
    else if (n == 4) ++s.quads;
    else if (n > 4) ++s.ngons;
    if (n >= 3) {
      for (std::uint32_t k = 1; k + 1 < n; ++k) {
        const float *a = P(m, c[0]), *bb = P(m, c[k]), *cc = P(m, c[k + 1]);
        const double ux = double(bb[0]) - a[0], uy = double(bb[1]) - a[1], uz = double(bb[2]) - a[2];
        const double vx = double(cc[0]) - a[0], vy = double(cc[1]) - a[1], vz = double(cc[2]) - a[2];
        const double x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
        s.surface_area += 0.5 * std::sqrt(x * x + y * y + z * z);
      }
    }
    if (n == 4) {
      double emin = std::numeric_limits<double>::infinity(), emax = 0, dev = 0;
      for (std::uint32_t k = 0; k < 4; ++k) {
        const float *p = P(m, c[k]), *pn = P(m, c[(k + 1) % 4]), *pp = P(m, c[(k + 3) % 4]);
        const double l = dist(p, pn);
        emin = std::min(emin, l);
        emax = std::max(emax, l);
        const double ax = double(pn[0]) - p[0], ay = double(pn[1]) - p[1], az = double(pn[2]) - p[2];
        const double bx = double(pp[0]) - p[0], by = double(pp[1]) - p[1], bz = double(pp[2]) - p[2];
        const double la = std::sqrt(ax * ax + ay * ay + az * az), lb = std::sqrt(bx * bx + by * by + bz * bz);
        if (la > 0 && lb > 0) {
          const double cs = std::clamp((ax * bx + ay * by + az * bz) / (la * lb), -1.0, 1.0);
          dev = std::max(dev, std::fabs(std::acos(cs) * 180.0 / kPi - 90.0));
        }
      }
      const double aspect = emin > 0 ? emax / emin : std::numeric_limits<double>::infinity();
      s.quad_aspect.min = std::min(s.quad_aspect.min, aspect);
      s.quad_aspect.max = std::max(s.quad_aspect.max, aspect);
      aspect_sum += aspect;
      dev_sum += dev;
      s.quad_angle_dev_max_deg = std::max(s.quad_angle_dev_max_deg, dev);
      ++quad_n;
    }
  }
  if (quad_n) {
    s.quad_aspect.mean = aspect_sum / static_cast<double>(quad_n);
    s.quad_angle_dev_mean_deg = dev_sum / static_cast<double>(quad_n);
  } else {
    s.quad_aspect = {};
  }
  s.max_quad_nonplanarity = detail::max_quad_nonplanarity(m);
  s.quad_ratio = s.faces ? static_cast<double>(s.quads) / static_cast<double>(s.faces) : 0.0;
  for (const std::uint8_t r : referenced) s.referenced_vertices += r;

  // Unique edges, valence, boundary vertices.
  std::sort(edges.begin(), edges.end());
  std::vector<std::uint32_t> valence(s.vertices, 0);
  std::vector<std::uint8_t> boundary(s.vertices, 0);
  s.edge_length.min = std::numeric_limits<double>::infinity();
  double len_sum = 0;
  for (std::size_t i = 0; i < edges.size();) {
    std::size_t j = i + 1;
    while (j < edges.size() && edges[j] == edges[i]) ++j;
    ++s.unique_edges;
    ++valence[edges[i].lo];
    ++valence[edges[i].hi];
    if (j - i == 1) boundary[edges[i].lo] = boundary[edges[i].hi] = 1;
    const double l = dist(P(m, edges[i].lo), P(m, edges[i].hi));
    s.edge_length.min = std::min(s.edge_length.min, l);
    s.edge_length.max = std::max(s.edge_length.max, l);
    len_sum += l;
    i = j;
  }
  if (s.unique_edges) s.edge_length.mean = len_sum / static_cast<double>(s.unique_edges);
  else s.edge_length = {};
  for (std::size_t v = 0; v < s.vertices; ++v) {
    if (valence[v] == 0) continue;
    ++s.valence_histogram[std::min<std::size_t>(valence[v], 16)];
    if (boundary[v] ? valence[v] != 3 : valence[v] != 4) ++s.irregular_vertices;
  }
  return s;
}

// ------------------------------------------------------------------------------------------ compare

namespace {

Check make_check(std::string name, double value, double limit, std::string note = {}) {
  Check c;
  c.name = std::move(name);
  c.value = value;
  c.limit = limit;
  c.pass = value <= limit;
  c.note = std::move(note);
  return c;
}

bool same_bits(float a, float b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }

std::size_t count_bit_differences(const std::vector<float>& a, const std::vector<float>& b) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.size(); ++i) n += same_bits(a[i], b[i]) ? 0 : 1;
  return n;
}

bool same_connectivity(const PolyMesh& a, const PolyMesh& b) { return a.face_offsets == b.face_offsets && a.corners == b.corners; }

struct PosKey {
  std::uint32_t x, y, z;
  bool operator==(const PosKey& o) const noexcept { return x == o.x && y == o.y && z == o.z; }
};
struct PosHash {
  std::size_t operator()(const PosKey& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint32_t w : {k.x, k.y, k.z}) {
      h ^= w;
      h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
  }
};
PosKey key_of(const PolyMesh& m, std::size_t v) {
  PosKey k;
  std::memcpy(&k.x, &m.positions[3 * v], 4);
  std::memcpy(&k.y, &m.positions[3 * v + 1], 4);
  std::memcpy(&k.z, &m.positions[3 * v + 2], 4);
  return k;
}

// Faces rotated to start at their smallest vertex, then sorted: a canonical form that ignores face order
// and the starting corner but keeps winding.
std::vector<std::vector<std::uint32_t>> canonical_faces(const PolyMesh& m, const std::vector<std::uint32_t>* remap) {
  std::vector<std::vector<std::uint32_t>> out;
  out.reserve(m.face_count());
  for (std::size_t f = 0; f < m.face_count(); ++f) {
    std::vector<std::uint32_t> face(m.corners.begin() + m.face_offsets[f], m.corners.begin() + m.face_offsets[f + 1]);
    if (remap) for (auto& v : face) v = (*remap)[v];
    std::rotate(face.begin(), std::min_element(face.begin(), face.end()), face.end());
    out.push_back(std::move(face));
  }
  std::sort(out.begin(), out.end());
  return out;
}

double max_displacement(const PolyMesh& a, const PolyMesh& b, const std::vector<std::uint32_t>* test_to_ref) {
  double worst = 0;
  const std::size_t n = a.vertex_count();
  for (std::size_t v = 0; v < n; ++v) {
    const std::size_t w = test_to_ref ? static_cast<std::size_t>((*test_to_ref)[v]) : v;
    worst = std::max(worst, dist(P(a, static_cast<std::uint32_t>(w)), P(b, static_cast<std::uint32_t>(v))));
  }
  return worst;
}

std::int64_t genus_total(const TopologyInfo& t, std::size_t& undefined) {
  std::int64_t g = 0;
  undefined = 0;
  for (const ComponentInfo& c : t.components) {
    if (c.genus) g += *c.genus; else ++undefined;
  }
  return g;
}

}  // namespace

Result<CompareReport> compare(const PolyMesh& ref, const PolyMesh& test, Tier tier, const DistanceOptions& opts) {
  if (auto r = check_indices(ref, "reference mesh"); !r) return r.error();
  if (auto r = check_indices(test, "test mesh"); !r) return r.error();
  CompareReport rep;
  rep.tier = tier;
  const BoundingBox bb = bounding_box(ref);
  const double d = bb.diagonal();

  if (tier == Tier::T0 || tier == Tier::T1) {
    rep.checks.push_back(make_check("vertex_count_difference", std::abs(double(ref.vertex_count()) - double(test.vertex_count())), 0));
    rep.checks.push_back(make_check("face_count_difference", std::abs(double(ref.face_count()) - double(test.face_count())), 0));
    const bool same_counts = ref.vertex_count() == test.vertex_count() && ref.face_count() == test.face_count();
    if (tier == Tier::T0) {
      rep.checks.push_back(make_check("position_bits_differing", same_counts ? double(count_bit_differences(ref.positions, test.positions)) : 1.0, 0));
      rep.checks.push_back(make_check("connectivity_differs", (same_counts && same_connectivity(ref, test)) ? 0.0 : 1.0, 0, "same order required"));
    } else {
      double disp = std::numeric_limits<double>::infinity();
      bool connectivity_ok = false;
      std::string how = "not equal";
      if (same_counts) {
        if (same_connectivity(ref, test)) {
          connectivity_ok = true;
          how = "same order";
          disp = max_displacement(ref, test, nullptr);
        } else {
          std::unordered_map<PosKey, std::uint32_t, PosHash> by_pos;
          bool unique = true;
          for (std::size_t v = 0; v < ref.vertex_count() && unique; ++v) unique = by_pos.emplace(key_of(ref, v), static_cast<std::uint32_t>(v)).second;
          std::vector<std::uint32_t> remap(test.vertex_count());
          bool all_found = unique;
          for (std::size_t v = 0; v < test.vertex_count() && all_found; ++v) {
            const auto it = by_pos.find(key_of(test, v));
            if (it == by_pos.end()) all_found = false; else remap[v] = it->second;
          }
          if (all_found && canonical_faces(ref, nullptr) == canonical_faces(test, &remap)) {
            connectivity_ok = true;
            how = "relabelled by exact position";
            disp = 0;
          }
        }
      }
      rep.checks.push_back(make_check("connectivity_differs", connectivity_ok ? 0.0 : 1.0, 0, how));
      rep.checks.push_back(make_check("max_vertex_displacement", disp, 1e-5 * d, "limit = 1e-5 * reference bbox diagonal"));
    }
  } else {
    const auto vr = validate(ref), vt = validate(test);
    if (vr.has_errors()) return make_error(Errc::InputInvalid, "reference mesh is invalid");
    if (vt.has_errors()) return make_error(Errc::InputInvalid, "test mesh is invalid");
    auto sr = compute_stats(ref), st = compute_stats(test);
    if (!sr) return sr.error();
    if (!st) return st.error();
    auto dist_rep = surface_distance(ref, test, opts);
    if (!dist_rep) return dist_rep.error();
    const double h = dist_rep.value().hausdorff();
    const double edge_limit = 0.25 * sr.value().edge_length.mean;
    rep.checks.push_back(make_check("hausdorff_vs_mean_edge", h, edge_limit, "limit = 0.25 * mean reference edge length"));
    rep.checks.push_back(make_check("hausdorff_vs_diagonal", h, 1e-2 * d, "limit = 1e-2 * reference bbox diagonal"));
    const double f_ref = double(sr.value().faces), f_test = double(st.value().faces);
    rep.checks.push_back(make_check("face_count_relative_difference", f_ref > 0 ? std::fabs(f_test - f_ref) / f_ref : f_test, 0.02));
    rep.checks.push_back(make_check("quad_ratio_difference", std::fabs(st.value().quad_ratio - sr.value().quad_ratio), 0.01));
    const double irr_ref = double(sr.value().irregular_vertices);
    rep.checks.push_back(make_check("irregular_vertex_difference", std::fabs(double(st.value().irregular_vertices) - irr_ref),
                                    std::max(0.1 * irr_ref, 4.0), "limit = max(10%, 4)"));
    std::size_t und_r = 0, und_t = 0;
    const std::int64_t g_r = genus_total(vr.topology, und_r), g_t = genus_total(vt.topology, und_t);
    rep.checks.push_back(make_check("genus_difference", std::fabs(double(g_r - g_t)), 0));
    rep.checks.push_back(make_check("undefined_genus_components_difference", std::fabs(double(und_r) - double(und_t)), 0));
    rep.checks.push_back(make_check("component_count_difference", std::fabs(double(vr.topology.component_count) - double(vt.topology.component_count)), 0));
    rep.checks.push_back(make_check("boundary_loop_difference", std::fabs(double(vr.topology.boundary_loops) - double(vt.topology.boundary_loops)), 0));
    auto flag = [](std::size_t n) { return n > 0; };
    rep.checks.push_back(make_check("nonmanifold_edge_flag_differs", flag(vr.topology.nonmanifold_edges.count) != flag(vt.topology.nonmanifold_edges.count) ? 1.0 : 0.0, 0));
    rep.checks.push_back(make_check("nonmanifold_vertex_flag_differs", flag(vr.topology.nonmanifold_vertices.count) != flag(vt.topology.nonmanifold_vertices.count) ? 1.0 : 0.0, 0));
    rep.checks.push_back(make_check("orientation_flag_differs", flag(vr.topology.orientation_inconsistent_edges.count) != flag(vt.topology.orientation_inconsistent_edges.count) ? 1.0 : 0.0, 0));
    rep.checks.push_back(make_check("max_quad_nonplanarity_bias", dist_rep.value().max_quad_nonplanarity, std::numeric_limits<double>::infinity(),
                                    "informational: bound on the 'both diagonals' bias in the Hausdorff value"));
  }
  rep.pass = std::all_of(rep.checks.begin(), rep.checks.end(), [](const Check& c) { return c.pass; });
  return rep;
}

// ------------------------------------------------------------------------------------------ JSON

namespace {
void json_string(std::string& o, const std::string& s) {
  o += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          o += "\\u00";
          o += hex[c >> 4];
          o += hex[c & 15];
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  o += '"';
}
void json_number(std::string& o, double v) {
  if (!std::isfinite(v)) {
    o += "null";
    return;
  }
  char buf[40];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  o.append(buf, r.ptr);
}
}  // namespace

std::string CompareReport::to_json() const {
  std::string o = "{\"tier\":";
  json_string(o, to_string(tier));
  o += ",\"pass\":";
  o += pass ? "true" : "false";
  o += ",\"checks\":[";
  for (std::size_t i = 0; i < checks.size(); ++i) {
    const Check& c = checks[i];
    if (i) o += ',';
    o += "{\"name\":";
    json_string(o, c.name);
    o += ",\"value\":";
    json_number(o, c.value);
    o += ",\"limit\":";
    json_number(o, c.limit);
    o += ",\"pass\":";
    o += c.pass ? "true" : "false";
    if (!c.note.empty()) {
      o += ",\"note\":";
      json_string(o, c.note);
    }
    o += '}';
  }
  o += "]}";
  return o;
}

}  // namespace rdesk
