#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include "doctest/doctest.h"
#include "fixtures.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/obj.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;
using namespace rdesk::io;
using namespace fixtures;

namespace {

const std::filesystem::path kData = std::filesystem::path(RDESK_TEST_DATA_DIR);

ObjFile load(const char* name) {
  auto r = read_obj_file(kData / name);
  REQUIRE_MESSAGE(r.ok(), name << ": " << (r.ok() ? "" : r.error().message));
  return std::move(r).value();
}

// Deterministic generator, so the tests do not depend on the standard library's distributions.
struct Lcg {
  std::uint64_t s;
  std::uint32_t next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(s >> 32);
  }
};

float random_finite_float(Lcg& g) {
  for (;;) {
    const std::uint32_t bits = g.next();
    float f;
    std::memcpy(&f, &bits, 4);
    if (std::isfinite(f)) return f;
  }
}

}  // namespace

TEST_CASE("obj: negative (relative) indices") {
  const ObjFile f = load("negative_indices.obj");
  CHECK(f.mesh.vertex_count() == 5);
  CHECK(f.mesh.face_offsets == std::vector<std::uint32_t>{0, 4, 7, 10});
  CHECK(f.mesh.corners == std::vector<std::uint32_t>{0, 1, 2, 3, 0, 1, 4, 4, 2, 1});
}

TEST_CASE("obj: n-gons are kept, not triangulated") {
  const ObjFile f = load("ngons.obj");
  CHECK(f.mesh.vertex_count() == 9);
  REQUIRE(f.mesh.face_count() == 4);
  CHECK(f.mesh.face_size(0) == 5);
  CHECK(f.mesh.face_size(1) == 4);
  CHECK(f.mesh.face_size(2) == 5);
  CHECK(f.mesh.face_size(3) == 3);
  CHECK(f.mesh.corners[0] == 0);
  CHECK(f.warnings.empty());
}

TEST_CASE("obj: per-corner vt/vn are split into unique vertices") {
  const ObjFile f = load("vt_vn_per_corner.obj");
  // Tuples (v,vt,vn): (0,0,0) (0,0,1) (1,1,0) (2,2,0) (2,2,1) (3,3,1) in ascending order.
  REQUIRE(f.mesh.vertex_count() == 6);
  CHECK(f.mesh.corners == std::vector<std::uint32_t>{0, 2, 3, 1, 4, 5});
  REQUIRE(f.mesh.attrs.normals.size() == 18);
  CHECK(f.mesh.attrs.normals[2] == 1.0f);    // vertex 0: vn 1
  CHECK(f.mesh.attrs.normals[5] == -1.0f);   // vertex 1: vn 2
  CHECK(f.mesh.attrs.uv_dim[0] == 2);
  REQUIRE(f.mesh.attrs.uv[0].size() == 12);
  CHECK(f.mesh.attrs.uv[0][2 * 3] == 1.0f);  // vertex 3 = (2,2,0): vt (1,1)
  CHECK(f.mesh.attrs.uv[0][2 * 3 + 1] == 1.0f);
  // Positions of the split copies are identical.
  CHECK(f.mesh.positions[0] == f.mesh.positions[3]);
  CHECK(validate(f.mesh).format_errors.empty());
  CHECK(f.warnings.empty());
}

TEST_CASE("obj: no normals and no texture coordinates -> v numbering is kept") {
  const ObjFile f = load("cube_no_normals.obj");
  CHECK(f.mesh.vertex_count() == 8);
  CHECK(f.mesh.face_count() == 6);
  CHECK(f.mesh.attrs.normals.empty());
  CHECK(f.mesh.attrs.uv_dim[0] == 0);
  CHECK(f.mesh.corners[0] == 0);
  CHECK(f.mesh.corners[1] == 3);
  CHECK(signed_volume(f.mesh) == doctest::Approx(1.0));
  CHECK(validate(f.mesh).is_closed_manifold());
  CHECK(f.mesh.convention == Convention::RightHandedYUpCCW);
}

TEST_CASE("obj: CRLF files read exactly like LF files") {
  const ObjFile lf = load("cube_no_normals.obj");
  const ObjFile crlf = load("cube_crlf.obj");
  CHECK(bit_equal(lf.mesh.positions, crlf.mesh.positions));
  CHECK(lf.mesh.corners == crlf.mesh.corners);
  CHECK(lf.mesh.face_offsets == crlf.mesh.face_offsets);
}

TEST_CASE("obj: comments, o/g/usemtl -> face_group") {
  const ObjFile f = load("groups_materials.obj");
  REQUIRE(f.mesh.face_count() == 3);
  REQUIRE(f.mesh.face_group.size() == 3);
  // Three distinct (shape, material) pairs in order of first appearance.
  CHECK(f.mesh.face_group == std::vector<std::uint32_t>{0, 1, 2});
  REQUIRE(f.group_labels.size() == 3);
  CHECK(f.group_labels[0].find("red") != std::string::npos);
  CHECK(f.group_labels[1].find("blue") != std::string::npos);
  CHECK(f.group_labels[2].find("red") != std::string::npos);
  CHECK(f.group_labels[0] != f.group_labels[2]);  // same material, different group name
}

TEST_CASE("obj: the same material under the same group name is one group") {
  const std::string text =
      "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
      "g a\nusemtl m1\nf 1 2 3\nusemtl m2\nf 1 3 4\nusemtl m1\nf 1 2 4\n";
  auto r = read_obj(text);
  REQUIRE(r.ok());
  CHECK(r.value().mesh.face_group == std::vector<std::uint32_t>{0, 1, 0});
  CHECK(r.value().group_labels.size() == 2);
}

TEST_CASE("obj: the source convention is recorded, never applied") {
  ObjReadOptions o;
  o.source_convention = Convention::UnityLeftHandedYUpCW;
  auto r = read_obj_file(kData / "cube_no_normals.obj", o);
  REQUIRE(r.ok());
  CHECK(r.value().mesh.convention == Convention::UnityLeftHandedYUpCW);
  CHECK(r.value().mesh.positions[3] == 1.0f);  // untouched
}

TEST_CASE("obj: write then read is bit-exact for positions and exact for faces") {
  const std::vector<PolyMesh> inputs = {make_cube(), triangulate(make_cube()), make_torus(7, 5), make_chiral()};
  for (const PolyMesh& m : inputs) {
    auto text = serialize_obj(m);
    REQUIRE(text.ok());
    CHECK(text.value().find('\r') == std::string::npos);
    auto back = read_obj(text.value());
    REQUIRE(back.ok());
    CHECK(bit_equal(back.value().mesh.positions, m.positions));
    CHECK(back.value().mesh.face_offsets == m.face_offsets);
    CHECK(back.value().mesh.corners == m.corners);
  }
}

TEST_CASE("obj: 100000 random float bit patterns survive the text round trip") {
  Lcg g{12345};
  PolyMesh m;
  const std::size_t n = 100002;  // a multiple of 3
  for (std::size_t i = 0; i < n * 3; ++i) m.positions.push_back(random_finite_float(g));
  // Make sure the awkward values are in there: signed zeros, denormals, extremes.
  const float specials[] = {0.0f, -0.0f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
                            std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::lowest(), 1e-38f, 16777216.0f, 0.1f, 1.0f / 3.0f};
  for (std::size_t i = 0; i < sizeof specials / sizeof *specials; ++i) m.positions[i] = specials[i];
  m.face_offsets.push_back(0);
  for (std::uint32_t i = 0; i + 2 < n; i += 3) {
    m.corners.insert(m.corners.end(), {i, i + 1, i + 2});
    m.face_offsets.push_back(static_cast<std::uint32_t>(m.corners.size()));
  }
  auto text = serialize_obj(m);
  REQUIRE(text.ok());
  auto back = read_obj(text.value());
  REQUIRE(back.ok());
  REQUIRE(back.value().mesh.positions.size() == m.positions.size());
  std::size_t bad = 0, first_bad = 0;
  for (std::size_t i = 0; i < m.positions.size(); ++i) {
    if (std::memcmp(&m.positions[i], &back.value().mesh.positions[i], 4) != 0) {
      if (bad++ == 0) first_bad = i;
    }
  }
  CHECK_MESSAGE(bad == 0, bad << " floats differ; first at " << first_bad << ": wrote " << m.positions[first_bad]
                             << " read " << back.value().mesh.positions[first_bad]);
}

TEST_CASE("obj: uv and normals round trip") {
  PolyMesh m = make_cube();
  const std::size_t nv = m.vertex_count();
  for (std::size_t i = 0; i < nv; ++i) {
    m.attrs.normals.insert(m.attrs.normals.end(), {0.1f * static_cast<float>(i), -0.25f, 1.0f / 3.0f});
    m.attrs.uv[0].insert(m.attrs.uv[0].end(), {static_cast<float>(i) / 8.0f, 0.7f});
  }
  m.attrs.uv_dim[0] = 2;
  auto text = serialize_obj(m);
  REQUIRE(text.ok());
  auto back = read_obj(text.value());
  REQUIRE(back.ok());
  CHECK(bit_equal(back.value().mesh.positions, m.positions));
  CHECK(bit_equal(back.value().mesh.attrs.normals, m.attrs.normals));
  CHECK(bit_equal(back.value().mesh.attrs.uv[0], m.attrs.uv[0]));
  CHECK(back.value().mesh.corners == m.corners);
}

TEST_CASE("obj: a Unity-convention mesh is written as right-handed, and the reader gets the same geometry") {
  const PolyMesh rh = make_chiral();
  const PolyMesh unity = to_convention(rh, Convention::UnityLeftHandedYUpCW);
  auto text = serialize_obj(unity);  // default target: right-handed
  REQUIRE(text.ok());
  auto back = read_obj(text.value());
  REQUIRE(back.ok());
  CHECK(bit_equal(back.value().mesh.positions, rh.positions));
  CHECK(back.value().mesh.corners == rh.corners);
  CHECK(signed_volume(back.value().mesh) > 0.0);

  ObjWriteOptions keep;
  keep.target = Convention::UnityLeftHandedYUpCW;
  auto raw = serialize_obj(unity, keep);
  REQUIRE(raw.ok());
  CHECK(raw.value() != text.value());
}

TEST_CASE("obj: face groups become g lines, ascending, with stable order inside a group") {
  PolyMesh m = make_cube();
  m.face_group = {1, 0, 1, 0, 2, 2};
  ObjWriteOptions o;
  const std::vector<std::string> labels = {"top", "", "side"};
  o.group_labels = &labels;
  auto text = serialize_obj(m, o);
  REQUIRE(text.ok());
  const std::string& t = text.value();
  const auto g0 = t.find("g top"), g1 = t.find("g group_1"), g2 = t.find("g side");
  REQUIRE(g0 != std::string::npos);
  REQUIRE(g1 != std::string::npos);
  REQUIRE(g2 != std::string::npos);
  CHECK(g0 < g1);
  CHECK(g1 < g2);
  auto back = read_obj(t);
  REQUIRE(back.ok());
  CHECK(back.value().mesh.face_group == std::vector<std::uint32_t>{0, 0, 1, 1, 2, 2});
  // Faces 1 and 3 (group 0) come first, in their original order.
  CHECK(back.value().mesh.corners[0] == m.corners[4]);
}

TEST_CASE("obj: no g lines when there is no face_group; comments are prefixed") {
  ObjWriteOptions o;
  o.comment = "line one\nline two";
  auto text = serialize_obj(make_cube(), o);
  REQUIRE(text.ok());
  CHECK(text.value().find("\ng ") == std::string::npos);
  CHECK(text.value().find("# line one\n# line two\n") != std::string::npos);
}

TEST_CASE("obj: reader errors") {
  SUBCASE("face index out of range") {
    auto r = read_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == Errc::Parse);
  }
  SUBCASE("texcoord and normal indices out of range") {
    CHECK_FALSE(read_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/5 2/5 3/5\n").ok());
    CHECK_FALSE(read_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1//5 2//5 3//5\n").ok());
  }
  SUBCASE("garbage never crashes") {
    for (const char* s : {"", "\n\n", "f", "f 1", "v", "v 1 2", "vt", "f 1/1/1/1 2 3", "f 0 0 0", "f -9 -9 -9",
                          "\xff\xfe\x00\x01", "usemtl", "usemtl \n", "g\no\n", "v 1e999 0 0\nv 0 0 0\nv 0 1 0\nf 1 2 3\n"}) {
      auto r = read_obj(std::string(s, std::strlen(s)));
      if (r.ok()) {
        const ValidationReport v = validate(r.value().mesh);
        CHECK(v.format_errors.empty());
        CHECK(v.index_out_of_range_faces.count == 0);
      }
    }
  }
  SUBCASE("missing file") {
    CHECK(read_obj_file(kData / "nope.obj").error().code == Errc::IoRead);
  }
}

TEST_CASE("obj: degenerate faces are skipped with a warning") {
  auto r = read_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2\nf 1 2 3\n");
  REQUIRE(r.ok());
  CHECK(r.value().mesh.face_count() == 1);
}

TEST_CASE("obj: unreferenced vertices are kept, in every code path") {
  auto a = read_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 9 9 9\nf 1 2 3\n");
  REQUIRE(a.ok());
  CHECK(a.value().mesh.vertex_count() == 4);
  auto b = read_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 9 9 9\nvn 0 0 1\nf 1//1 2//1 3//1\n");
  REQUIRE(b.ok());
  REQUIRE(b.value().mesh.vertex_count() == 4);
  CHECK(b.value().mesh.positions[9] == 9.0f);
  CHECK(!b.value().warnings.empty());  // vertex 3 has no normal
}

TEST_CASE("obj: writer refuses what it cannot write faithfully") {
  PolyMesh m = make_cube();
  SUBCASE("index out of range") { m.corners[0] = 8; }
  SUBCASE("NaN position") { m.positions[0] = std::numeric_limits<float>::quiet_NaN(); }
  SUBCASE("infinite normal") { m.attrs.normals.assign(24, std::numeric_limits<float>::infinity()); }
  SUBCASE("malformed container") { m.face_offsets.back() = 1; }
  CHECK(serialize_obj(m).error().code == Errc::InputInvalid);
}

TEST_CASE("obj: files on disk, atomic write") {
  const auto dir = std::filesystem::temp_directory_path() / "rdesk_test_obj";
  std::filesystem::create_directories(dir);
  const auto path = dir / "out.obj";
  REQUIRE(write_obj_file(path, make_cube()).ok());
  CHECK_FALSE(std::filesystem::exists(dir / "out.obj.tmp"));
  auto back = read_obj_file(path);
  REQUIRE(back.ok());
  CHECK(back.value().mesh.face_count() == 6);
  CHECK(write_obj_file(dir / "no_such_dir" / "x.obj", make_cube()).error().code == Errc::IoWrite);
  std::filesystem::remove_all(dir);
}

// Performance target (A4 exit): a 1M-triangle OBJ loads in under 3 s in a Release build. Not a gate: it only
// reports. Set RDESK_PERF=1 to run the full size; otherwise a 50k-triangle smoke version runs.
TEST_CASE("obj: large file smoke / timing") {
  const bool full = env_value("RDESK_PERF").has_value();
  const std::uint32_t n = full ? 708 : 160;  // n x n quads -> 2 n^2 triangles
  std::string text;
  text.reserve(static_cast<std::size_t>(n) * n * 60);
  for (std::uint32_t y = 0; y <= n; ++y) {
    for (std::uint32_t x = 0; x <= n; ++x) {
      text += "v " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string((x * 7 + y * 13) % 17) + "\n";
    }
  }
  const std::uint32_t row = n + 1;
  for (std::uint32_t y = 0; y < n; ++y) {
    for (std::uint32_t x = 0; x < n; ++x) {
      const std::uint32_t a = y * row + x + 1, b = a + 1, c = a + row + 1, d = a + row;
      text += "f " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c) + "\n";
      text += "f " + std::to_string(a) + " " + std::to_string(c) + " " + std::to_string(d) + "\n";
    }
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto r = read_obj(text);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(r.ok());
  CHECK(r.value().mesh.face_count() == static_cast<std::size_t>(2) * n * n);
  CHECK(r.value().mesh.vertex_count() == static_cast<std::size_t>(row) * row);
  std::cout << "[perf] read_obj: " << r.value().mesh.face_count() << " triangles, " << text.size() / (1024 * 1024)
            << " MiB in " << secs << " s" << (full ? "" : " (smoke size)") << "\n";
}

TEST_CASE("obj: vw (skin weight) lines are ignored, even absurd ones, and do not disturb the rest") {
  // tinyobjloader v2.0.0rc13 casts the joint id of a `vw` line to int without a range check (UB for huge values).
  const std::string text =
      "v 0 0 0\nv 1 0 0\nvw 0 99999999999999999999999999999999999999999999 0.5\nv 0 1 0\n"
      "  vw 1 3333333333333333333333333333333333333333 1\r\nvwx 1 2 3\nf 1 2 3\n";
  auto r = read_obj(text);
  REQUIRE(r.ok());
  CHECK(r.value().mesh.vertex_count() == 3);
  CHECK(r.value().mesh.face_count() == 1);
  // "vwx" is not a vw line; tinyobjloader treats it as an unknown keyword and ignores it.
  const auto plain = read_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
  REQUIRE(plain.ok());
  CHECK(bit_equal(r.value().mesh.positions, plain.value().mesh.positions));
}

TEST_CASE("obj: usemtl handling survives the single-pass preparation (CRLF, leading blanks, no trailing newline)") {
  const std::string text = "v 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\n  usemtl\tshiny metal  \r\nf 1 2 3\r\nusemtl other\r\nf 1 3 2";
  auto r = read_obj(text);
  REQUIRE(r.ok());
  REQUIRE(r.value().mesh.face_group.size() == 2);
  CHECK(r.value().mesh.face_group[0] != r.value().mesh.face_group[1]);
  CHECK(r.value().group_labels[0].find("shiny metal") != std::string::npos);
}

TEST_CASE("obj: material names with spaces stay distinct (tinyobjloader alone would cut them at the first blank)") {
  const std::string text =
      "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Mat A\nf 1 2 3\nusemtl Mat B\nf 1 3 2\nusemtl Mat  A\nf 2 3 1\nusemtl Mat A\nf 3 1 2\n";
  auto r = read_obj(text);
  REQUIRE(r.ok());
  // "Mat A", "Mat B", "Mat  A" (two blanks) are three different materials; the fourth face reuses the first.
  CHECK(r.value().mesh.face_group == std::vector<std::uint32_t>{0, 1, 2, 0});
  REQUIRE(r.value().group_labels.size() == 3);
  CHECK(r.value().group_labels[0] == "|Mat A");
  CHECK(r.value().group_labels[1] == "|Mat B");
  CHECK(r.value().group_labels[2] == "|Mat  A");
}
