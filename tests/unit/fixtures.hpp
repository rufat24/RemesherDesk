// Mesh fixtures shared by the unit tests. Header-only.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "rdesk/convention.hpp"
#include "rdesk/mesh.hpp"

namespace fixtures {

using rdesk::PolyMesh;

inline PolyMesh make_mesh(const std::vector<float>& pos,
                          const std::vector<std::vector<std::uint32_t>>& faces) {
  PolyMesh m;
  m.positions = pos;
  m.face_offsets.push_back(0);
  for (const auto& f : faces) {
    m.corners.insert(m.corners.end(), f.begin(), f.end());
    m.face_offsets.push_back(static_cast<std::uint32_t>(m.corners.size()));
  }
  return m;
}

// Axis-aligned cube [o, o+s]^3 in the right-handed convention; every face's cross product points outward.
inline PolyMesh make_cube(float ox = 0, float oy = 0, float oz = 0, float s = 1) {
  const std::vector<float> pos = {
      ox,     oy,     oz,      // 0
      ox + s, oy,     oz,      // 1
      ox + s, oy + s, oz,      // 2
      ox,     oy + s, oz,      // 3
      ox,     oy,     oz + s,  // 4
      ox + s, oy,     oz + s,  // 5
      ox + s, oy + s, oz + s,  // 6
      ox,     oy + s, oz + s,  // 7
  };
  return make_mesh(pos, {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}});
}

// Concatenate (no welding).
inline PolyMesh append(PolyMesh a, const PolyMesh& b) {
  const auto base = static_cast<std::uint32_t>(a.vertex_count());
  a.positions.insert(a.positions.end(), b.positions.begin(), b.positions.end());
  const auto cbase = static_cast<std::uint32_t>(a.corners.size());
  for (const auto c : b.corners) a.corners.push_back(c + base);
  for (std::size_t f = 1; f < b.face_offsets.size(); ++f) a.face_offsets.push_back(b.face_offsets[f] + cbase);
  return a;
}

// Split every polygon into a fan of triangles.
inline PolyMesh triangulate(const PolyMesh& m) {
  PolyMesh t;
  t.positions = m.positions;
  t.convention = m.convention;
  t.face_offsets.push_back(0);
  for (std::size_t f = 0; f < m.face_count(); ++f) {
    const std::size_t b = m.face_offsets[f], e = m.face_offsets[f + 1];
    for (std::size_t c = b + 1; c + 1 < e; ++c) {
      t.corners.push_back(m.corners[b]);
      t.corners.push_back(m.corners[c]);
      t.corners.push_back(m.corners[c + 1]);
      t.face_offsets.push_back(static_cast<std::uint32_t>(t.corners.size()));
    }
  }
  return t;
}

// Quad torus (genus 1), nu x nv quads, major radius 3, minor radius 1, Y up.
inline PolyMesh make_torus(std::uint32_t nu = 6, std::uint32_t nv = 4) {
  std::vector<float> pos;
  const double pi = 3.14159265358979323846;
  for (std::uint32_t i = 0; i < nu; ++i) {
    for (std::uint32_t j = 0; j < nv; ++j) {
      const double u = 2 * pi * i / nu, v = 2 * pi * j / nv;
      const double r = 3.0 + std::cos(v);
      pos.push_back(static_cast<float>(r * std::cos(u)));
      pos.push_back(static_cast<float>(std::sin(v)));
      pos.push_back(static_cast<float>(r * std::sin(u)));
    }
  }
  auto id = [&](std::uint32_t i, std::uint32_t j) { return (i % nu) * nv + (j % nv); };
  std::vector<std::vector<std::uint32_t>> faces;
  for (std::uint32_t i = 0; i < nu; ++i) {
    for (std::uint32_t j = 0; j < nv; ++j) {
      faces.push_back({id(i, j), id(i, j + 1), id(i + 1, j + 1), id(i + 1, j)});
    }
  }
  return make_mesh(pos, faces);
}

// Two tetrahedra forming an asymmetric "L": not equal to their own X mirror.
inline PolyMesh make_chiral() {
  // Tetra 1 at the origin with legs 1, 2, 3; tetra 2 offset in +X, +Y with different legs.
  const std::vector<float> pos = {
      0, 0, 0,  1, 0, 0,  0, 2, 0,  0, 0, 3,         // tetra 1 (vertices 0..3)
      4, 1, 0,  6, 1, 0,  4, 2, 0,  4, 1, 5,         // tetra 2 (vertices 4..7)
  };
  // Outward-facing triangles for both (checked via signed volume in the tests).
  return make_mesh(pos, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3},
                         {4, 6, 5}, {4, 5, 7}, {4, 7, 6}, {5, 6, 7}});
}

// Portable getenv: MSVC deprecates std::getenv (C4996, an error under /WX).
inline std::optional<std::string> env_value(const char* name) {
#ifdef _MSC_VER
  char* buf = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return std::nullopt;
  std::string v(buf);
  std::free(buf);
  return v;
#else
  const char* v = std::getenv(name);
  if (v == nullptr) return std::nullopt;
  return std::string(v);
#endif
}

inline bool bit_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

}  // namespace fixtures

namespace fixtures {

// nx x ny quads in the XY plane (z = 0), cell size `cell`, normals +z (right-handed, counter-clockwise).
inline rdesk::PolyMesh make_grid(std::uint32_t nx, std::uint32_t ny, float cell = 1.0f, float z = 0.0f) {
  std::vector<float> pos;
  for (std::uint32_t y = 0; y <= ny; ++y) {
    for (std::uint32_t x = 0; x <= nx; ++x) {
      pos.insert(pos.end(), {static_cast<float>(x) * cell, static_cast<float>(y) * cell, z});
    }
  }
  std::vector<std::vector<std::uint32_t>> faces;
  const std::uint32_t row = nx + 1;
  for (std::uint32_t y = 0; y < ny; ++y) {
    for (std::uint32_t x = 0; x < nx; ++x) {
      const std::uint32_t a = y * row + x;
      faces.push_back({a, a + 1, a + row + 1, a + row});
    }
  }
  return make_mesh(pos, faces);
}

}  // namespace fixtures
