#include "rdesk/validate.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace rdesk {

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

// Disjoint sets with path halving and union by size.
class Dsu {
 public:
  explicit Dsu(std::size_t n) : parent_(n), size_(n, 1) {
    for (std::size_t i = 0; i < n; ++i) parent_[i] = static_cast<std::uint32_t>(i);
  }
  std::uint32_t find(std::uint32_t x) noexcept {
    while (parent_[x] != x) {
      parent_[x] = parent_[parent_[x]];
      x = parent_[x];
    }
    return x;
  }
  void unite(std::uint32_t a, std::uint32_t b) noexcept {
    a = find(a);
    b = find(b);
    if (a == b) return;
    if (size_[a] < size_[b]) std::swap(a, b);
    parent_[b] = a;
    size_[a] += size_[b];
  }

 private:
  std::vector<std::uint32_t> parent_;
  std::vector<std::uint32_t> size_;
};

}  // namespace

std::vector<std::string> format_errors(const PolyMesh& m) {
  std::vector<std::string> e;
  if (m.positions.size() % 3 != 0) e.emplace_back("positions size is not a multiple of 3");
  if (m.face_offsets.empty()) {
    if (!m.corners.empty()) e.emplace_back("corners present but face_offsets is empty");
  } else {
    if (m.face_offsets.front() != 0) e.emplace_back("face_offsets[0] != 0");
    if (m.face_offsets.back() != m.corners.size()) {
      e.emplace_back("face_offsets.back() != corners.size()");
    }
    for (std::size_t f = 0; f + 1 < m.face_offsets.size(); ++f) {
      if (m.face_offsets[f] > m.face_offsets[f + 1]) {
        e.emplace_back("face_offsets is not monotonic at face " + std::to_string(f));
        break;
      }
    }
  }
  if (!m.face_group.empty() && m.face_group.size() != m.face_count()) {
    e.emplace_back("face_group size != face count");
  }
  const std::size_t nv = m.vertex_count();
  auto check = [&](const std::vector<float>& v, std::size_t stride, const char* name) {
    if (!v.empty() && v.size() != nv * stride) {
      e.emplace_back(std::string(name) + " size != vertex_count * " + std::to_string(stride));
    }
  };
  check(m.attrs.normals, 3, "normals");
  check(m.attrs.tangents, 4, "tangents");
  check(m.attrs.colors, 4, "colors");
  for (std::size_t i = 0; i < m.attrs.uv.size(); ++i) {
    const std::size_t dim = m.attrs.uv_dim[i];
    if (m.attrs.uv[i].empty()) continue;
    if (dim < 2 || dim > 4) {
      e.emplace_back("uv" + std::to_string(i) + " has data but uv_dim is not 2..4");
    } else {
      check(m.attrs.uv[i], dim, "uv");
    }
  }
  return e;
}

Result<TopologyInfo> analyze_topology(const PolyMesh& m) {
  if (auto fe = format_errors(m); !fe.empty()) {
    return make_error(Errc::InputInvalid, "malformed mesh container: " + fe.front());
  }
  const std::size_t nv = m.vertex_count();
  const std::size_t nf = m.face_count();
  for (const std::uint32_t c : m.corners) {
    if (c >= nv) return make_error(Errc::InputInvalid, "corner index out of range");
  }
  if (m.corners.size() >= kNone) return make_error(Errc::InputInvalid, "too many corners (>= 2^32-1)");

  TopologyInfo t;

  // Directed half-edges. `c0`/`c1` are the corners (indices into m.corners) at the start/end vertex.
  struct HalfEdge {
    std::uint32_t lo, hi, c0, c1;
    std::uint8_t dir;  // 0: traverses lo -> hi, 1: hi -> lo
  };
  std::vector<HalfEdge> he;
  he.reserve(m.corners.size());
  for (std::size_t f = 0; f < nf; ++f) {
    const std::size_t b = m.face_offsets[f];
    const std::size_t n = m.face_offsets[f + 1] - b;
    for (std::size_t k = 0; k < n; ++k) {
      const std::size_t c0 = b + k;
      const std::size_t c1 = b + (k + 1 == n ? 0 : k + 1);
      const std::uint32_t u = m.corners[c0], v = m.corners[c1];
      if (u == v) continue;  // degenerate edge inside a face
      he.push_back({std::min(u, v), std::max(u, v), static_cast<std::uint32_t>(c0),
                    static_cast<std::uint32_t>(c1), static_cast<std::uint8_t>(u < v ? 0 : 1)});
    }
  }
  std::sort(he.begin(), he.end(), [](const HalfEdge& a, const HalfEdge& b) {
    if (a.lo != b.lo) return a.lo < b.lo;
    if (a.hi != b.hi) return a.hi < b.hi;
    return a.c0 < b.c0;
  });

  // Vertex connectivity -> components.
  Dsu vdsu(nv);
  std::vector<std::uint8_t> referenced(nv, 0);
  for (std::size_t f = 0; f < nf; ++f) {
    const std::size_t b = m.face_offsets[f], e = m.face_offsets[f + 1];
    for (std::size_t c = b; c < e; ++c) {
      referenced[m.corners[c]] = 1;
      if (c > b) vdsu.unite(m.corners[b], m.corners[c]);
    }
  }
  std::vector<std::uint32_t> comp_of_root(nv, kNone);
  for (std::size_t v = 0; v < nv; ++v) {
    if (!referenced[v]) continue;
    const std::uint32_t r = vdsu.find(static_cast<std::uint32_t>(v));
    if (comp_of_root[r] == kNone) {
      comp_of_root[r] = static_cast<std::uint32_t>(t.components.size());
      t.components.emplace_back();
    }
    ++t.components[comp_of_root[r]].vertices;
  }
  auto comp_of_vertex = [&](std::uint32_t v) { return comp_of_root[vdsu.find(v)]; };
  for (std::size_t f = 0; f < nf; ++f) {
    if (m.face_offsets[f] == m.face_offsets[f + 1]) continue;
    ++t.components[comp_of_vertex(m.corners[m.face_offsets[f]])].faces;
  }

  // Edge groups.
  Dsu cdsu(m.corners.size());  // corner fans, for non-manifold vertices
  Dsu bdsu(nv);                // boundary graph, for loops
  std::vector<std::uint8_t> on_boundary(nv, 0);
  std::vector<std::size_t> nm_edges_c(t.components.size(), 0), orient_bad_c(t.components.size(), 0),
      nm_verts_c(t.components.size(), 0);

  for (std::size_t i = 0; i < he.size();) {
    std::size_t j = i + 1;
    while (j < he.size() && he[j].lo == he[i].lo && he[j].hi == he[i].hi) ++j;
    const std::size_t cnt = j - i;
    const std::uint32_t lo = he[i].lo, hi = he[i].hi;
    const std::uint32_t comp = comp_of_vertex(lo);
    ++t.unique_edges;
    ++t.components[comp].edges;
    if (cnt == 1) {
      ++t.boundary_edges;
      ++t.components[comp].boundary_edges;
      on_boundary[lo] = on_boundary[hi] = 1;
      bdsu.unite(lo, hi);
    } else {
      if (cnt == 2) {
        if (he[i].dir == he[i + 1].dir) {
          t.orientation_inconsistent_edges.add({lo, hi});
          ++orient_bad_c[comp];
        }
        // Fans are joined across manifold edges only; faces around a non-manifold edge stay separate,
        // so that edge's end points are reported as non-manifold vertices too.
        auto corner_at = [](const HalfEdge& h, bool want_lo) {
          return (h.dir == 0) == want_lo ? h.c0 : h.c1;
        };
        cdsu.unite(corner_at(he[i], true), corner_at(he[i + 1], true));
        cdsu.unite(corner_at(he[i], false), corner_at(he[i + 1], false));
      } else {
        t.nonmanifold_edges.add({lo, hi});
        ++nm_edges_c[comp];
      }
    }
    i = j;
  }

  // Non-manifold vertices: a vertex whose corners fall into more than one fan.
  {
    std::vector<std::uint32_t> first_root(nv, kNone);
    std::vector<std::uint8_t> flagged(nv, 0);
    for (std::size_t c = 0; c < m.corners.size(); ++c) {
      const std::uint32_t v = m.corners[c];
      const std::uint32_t r = cdsu.find(static_cast<std::uint32_t>(c));
      if (first_root[v] == kNone) {
        first_root[v] = r;
      } else if (first_root[v] != r) {
        flagged[v] = 1;
      }
    }
    for (std::size_t v = 0; v < nv; ++v) {
      if (!flagged[v]) continue;
      t.nonmanifold_vertices.add(static_cast<std::uint32_t>(v));
      ++nm_verts_c[comp_of_vertex(static_cast<std::uint32_t>(v))];
    }
  }

  // Boundary loops: roots of the boundary graph, counted per component.
  for (std::size_t v = 0; v < nv; ++v) {
    if (!on_boundary[v]) continue;
    if (bdsu.find(static_cast<std::uint32_t>(v)) != v) continue;
    ++t.boundary_loops;
    ++t.components[comp_of_vertex(static_cast<std::uint32_t>(v))].boundary_loops;
  }

  // Per-component summary.
  t.component_count = t.components.size();
  for (std::size_t c = 0; c < t.components.size(); ++c) {
    ComponentInfo& ci = t.components[c];
    ci.euler = static_cast<std::int64_t>(ci.vertices) - static_cast<std::int64_t>(ci.edges) +
               static_cast<std::int64_t>(ci.faces);
    ci.closed = ci.boundary_edges == 0;
    ci.manifold = nm_edges_c[c] == 0 && nm_verts_c[c] == 0 && orient_bad_c[c] == 0;
    if (ci.closed && ci.manifold && ci.euler % 2 == 0) ci.genus = (2 - ci.euler) / 2;
    t.euler += ci.euler;
  }
  return t;
}

Result<std::int64_t> euler_characteristic(const PolyMesh& m) {
  auto t = analyze_topology(m);
  if (!t) return t.error();
  return t.value().euler;
}
Result<std::size_t> boundary_loops(const PolyMesh& m) {
  auto t = analyze_topology(m);
  if (!t) return t.error();
  return t.value().boundary_loops;
}
Result<std::size_t> components(const PolyMesh& m) {
  auto t = analyze_topology(m);
  if (!t) return t.error();
  return t.value().component_count;
}

ValidationReport validate(const PolyMesh& m) {
  ValidationReport r;
  r.format_errors = format_errors(m);
  r.vertex_count = m.vertex_count();
  r.face_count = m.face_count();
  r.corner_count = m.corners.size();
  if (!r.format_errors.empty()) return r;  // nothing else can be trusted

  const std::size_t nv = r.vertex_count;
  const BoundingBox bb = bounding_box(m);
  r.bbox_diagonal = bb.diagonal();

  for (std::size_t v = 0; v < nv; ++v) {
    if (!std::isfinite(m.positions[3 * v]) || !std::isfinite(m.positions[3 * v + 1]) ||
        !std::isfinite(m.positions[3 * v + 2])) {
      r.nonfinite_vertices.add(static_cast<std::uint32_t>(v));
    }
  }

  std::vector<std::uint8_t> referenced(nv, 0);
  const double area_eps = 1e-12 * r.bbox_diagonal * r.bbox_diagonal;
  std::vector<std::uint32_t> scratch;
  for (std::size_t f = 0; f < r.face_count; ++f) {
    const std::size_t b = m.face_offsets[f], e = m.face_offsets[f + 1], n = e - b;
    const auto fid = static_cast<std::uint32_t>(f);
    bool in_range = true;
    for (std::size_t c = b; c < e; ++c) {
      if (m.corners[c] >= nv) {
        in_range = false;
      } else {
        referenced[m.corners[c]] = 1;
      }
    }
    if (!in_range) r.index_out_of_range_faces.add(fid);
    if (n < 3) {
      r.faces_too_small.add(fid);
      continue;
    }
    if (!in_range) continue;

    scratch.assign(m.corners.begin() + static_cast<std::ptrdiff_t>(b),
                   m.corners.begin() + static_cast<std::ptrdiff_t>(e));
    std::sort(scratch.begin(), scratch.end());
    if (std::adjacent_find(scratch.begin(), scratch.end()) != scratch.end()) {
      r.repeated_vertex_faces.add(fid);
    }

    // Vector area (Newell, relative to corner 0): |sum cross(p_i - p_0, p_{i+1} - p_0)| / 2.
    const float* p0 = &m.positions[3 * static_cast<std::size_t>(m.corners[b])];
    double sx = 0, sy = 0, sz = 0;
    for (std::size_t c = b + 1; c + 1 < e; ++c) {
      const float* p1 = &m.positions[3 * static_cast<std::size_t>(m.corners[c])];
      const float* p2 = &m.positions[3 * static_cast<std::size_t>(m.corners[c + 1])];
      const double ux = double(p1[0]) - p0[0], uy = double(p1[1]) - p0[1], uz = double(p1[2]) - p0[2];
      const double vx = double(p2[0]) - p0[0], vy = double(p2[1]) - p0[1], vz = double(p2[2]) - p0[2];
      sx += uy * vz - uz * vy;
      sy += uz * vx - ux * vz;
      sz += ux * vy - uy * vx;
    }
    const double area = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
    if (area <= area_eps) r.zero_area_faces.add(fid);  // a NaN area compares false: not flagged
  }
  for (std::size_t v = 0; v < nv; ++v) {
    if (!referenced[v]) ++r.unreferenced_vertices;
  }

  if (r.index_out_of_range_faces.count == 0) {
    auto t = analyze_topology(m);
    if (t) {
      r.topology = std::move(t).value();
      r.topology_analyzed = true;
      if (r.topology.components.size() > kMaxReported) r.topology.components.resize(kMaxReported);
    }
  }
  return r;
}

// ------------------------------------------------------------------------------------ welding

namespace {

std::uint32_t canonical_bits(float f) noexcept {
  if (f == 0.0f) f = 0.0f;  // -0 -> +0
  std::uint32_t b;
  std::memcpy(&b, &f, sizeof b);
  return b;
}

struct BitKey {
  std::uint32_t x, y, z;
  bool operator==(const BitKey& o) const noexcept { return x == o.x && y == o.y && z == o.z; }
};
struct BitKeyHash {
  std::size_t operator()(const BitKey& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint32_t w : {k.x, k.y, k.z}) {
      h ^= w;
      h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
  }
};

struct CellKey {
  std::int64_t x, y, z;
  bool operator==(const CellKey& o) const noexcept { return x == o.x && y == o.y && z == o.z; }
};
struct CellKeyHash {
  std::size_t operator()(const CellKey& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::int64_t w : {k.x, k.y, k.z}) {
      h ^= static_cast<std::uint64_t>(w);
      h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
  }
};

// Keep only the vertices listed in `keep` (ascending old indices).
void compact(std::vector<float>& arr, std::size_t stride, const std::vector<std::uint32_t>& keep) {
  if (arr.empty()) return;
  std::vector<float> out;
  out.reserve(keep.size() * stride);
  for (const std::uint32_t old : keep) {
    const auto first = arr.begin() + static_cast<std::ptrdiff_t>(old * stride);
    out.insert(out.end(), first, first + static_cast<std::ptrdiff_t>(stride));
  }
  arr = std::move(out);
}

}  // namespace

Result<PolyMesh> weld_by_position(const PolyMesh& m, double eps) {
  if (!(eps >= 0.0) || !std::isfinite(eps)) {
    return make_error(Errc::ParamInvalid, "weld eps must be a finite number >= 0");
  }
  if (auto fe = format_errors(m); !fe.empty()) {
    return make_error(Errc::InputInvalid, "malformed mesh container: " + fe.front());
  }
  const std::size_t nv = m.vertex_count();
  for (const std::uint32_t c : m.corners) {
    if (c >= nv) return make_error(Errc::InputInvalid, "corner index out of range");
  }

  std::vector<std::uint32_t> remap(nv);   // old vertex -> new vertex
  std::vector<std::uint32_t> keep;        // new vertex -> old representative
  keep.reserve(nv);

  if (eps == 0.0) {
    std::unordered_map<BitKey, std::uint32_t, BitKeyHash> seen;
    seen.reserve(nv);
    for (std::size_t v = 0; v < nv; ++v) {
      const BitKey k{canonical_bits(m.positions[3 * v]), canonical_bits(m.positions[3 * v + 1]),
                     canonical_bits(m.positions[3 * v + 2])};
      auto [it, inserted] = seen.try_emplace(k, static_cast<std::uint32_t>(keep.size()));
      if (inserted) keep.push_back(static_cast<std::uint32_t>(v));
      remap[v] = it->second;
    }
  } else {
    const double limit = 4.0e18;
    std::unordered_map<CellKey, std::vector<std::uint32_t>, CellKeyHash> grid;  // cell -> new indices
    grid.reserve(nv);
    const double eps2 = eps * eps;
    for (std::size_t v = 0; v < nv; ++v) {
      const double p[3] = {m.positions[3 * v], m.positions[3 * v + 1], m.positions[3 * v + 2]};
      if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
        remap[v] = static_cast<std::uint32_t>(keep.size());
        keep.push_back(static_cast<std::uint32_t>(v));
        continue;
      }
      const double g[3] = {std::floor(p[0] / eps), std::floor(p[1] / eps), std::floor(p[2] / eps)};
      if (std::fabs(g[0]) > limit || std::fabs(g[1]) > limit || std::fabs(g[2]) > limit) {
        return make_error(Errc::ParamInvalid, "weld eps is too small for the coordinate range");
      }
      const CellKey cell{static_cast<std::int64_t>(g[0]), static_cast<std::int64_t>(g[1]),
                         static_cast<std::int64_t>(g[2])};
      std::uint32_t best = kNone;  // lowest new index within eps
      for (std::int64_t dx = -1; dx <= 1; ++dx) {
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
          for (std::int64_t dz = -1; dz <= 1; ++dz) {
            const auto it = grid.find(CellKey{cell.x + dx, cell.y + dy, cell.z + dz});
            if (it == grid.end()) continue;
            for (const std::uint32_t cand : it->second) {
              if (cand >= best) continue;
              const float* q = &m.positions[3 * static_cast<std::size_t>(keep[cand])];
              const double ex = p[0] - q[0], ey = p[1] - q[1], ez = p[2] - q[2];
              if (ex * ex + ey * ey + ez * ez <= eps2) best = cand;
            }
          }
        }
      }
      if (best != kNone) {
        remap[v] = best;
      } else {
        const auto id = static_cast<std::uint32_t>(keep.size());
        keep.push_back(static_cast<std::uint32_t>(v));
        grid[cell].push_back(id);
        remap[v] = id;
      }
    }
  }

  PolyMesh out;
  out.convention = m.convention;
  out.face_offsets = m.face_offsets;
  out.face_group = m.face_group;
  out.corners.reserve(m.corners.size());
  for (const std::uint32_t c : m.corners) out.corners.push_back(remap[c]);

  out.positions = m.positions;
  out.attrs = m.attrs;
  compact(out.positions, 3, keep);
  compact(out.attrs.normals, 3, keep);
  compact(out.attrs.tangents, 4, keep);
  compact(out.attrs.colors, 4, keep);
  for (std::size_t i = 0; i < out.attrs.uv.size(); ++i) {
    compact(out.attrs.uv[i], out.attrs.uv_dim[i], keep);
  }
  return out;
}

}  // namespace rdesk
