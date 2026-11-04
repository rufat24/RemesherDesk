#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "doctest/doctest.h"
#include "fixtures.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/rdm.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;
using namespace rdesk::io;
using namespace fixtures;
using Bytes = std::vector<std::uint8_t>;

namespace {

const std::filesystem::path kCube = std::filesystem::path(RDESK_TEST_DATA_DIR) / "cube_unity.rdm";

Bytes slurp(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---- hand-built files, for cases the writer cannot produce
void u32(Bytes& b, std::uint32_t v) { b.insert(b.end(), reinterpret_cast<std::uint8_t*>(&v), reinterpret_cast<std::uint8_t*>(&v) + 4); }
void u64(Bytes& b, std::uint64_t v) { b.insert(b.end(), reinterpret_cast<std::uint8_t*>(&v), reinterpret_cast<std::uint8_t*>(&v) + 8); }

Bytes chunk(const char* tag, const Bytes& payload, std::uint64_t claimed_len = ~0ull) {
  Bytes b(tag, tag + 4);
  u32(b, 0);
  u64(b, claimed_len == ~0ull ? payload.size() : claimed_len);
  b.insert(b.end(), payload.begin(), payload.end());
  b.insert(b.end(), (8 - payload.size() % 8) % 8, 0);
  return b;
}
Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes file(const std::vector<Bytes>& chunks) {
  Bytes b = {'R', 'D', 'M', 'F', 1, 0, 0, 0};
  u32(b, 0);
  u32(b, static_cast<std::uint32_t>(chunks.size()));
  for (const auto& c : chunks) b.insert(b.end(), c.begin(), c.end());
  return b;
}
std::string meta(std::uint64_t nv, std::uint64_t ns, const char* conv = "rh_yup_ccw") {
  return std::string("{\"format\":\"rdm\",\"version\":\"1.0\",\"convention\":\"") + conv +
         "\",\"vertex_count\":" + std::to_string(nv) + ",\"submesh_count\":" + std::to_string(ns) + "}";
}
Bytes floats(std::initializer_list<float> v) {
  Bytes b;
  for (float f : v) b.insert(b.end(), reinterpret_cast<std::uint8_t*>(&f), reinterpret_cast<std::uint8_t*>(&f) + 4);
  return b;
}
Bytes subm(std::uint32_t topo, std::initializer_list<std::uint32_t> idx, std::initializer_list<std::uint32_t> sizes = {}) {
  Bytes b;
  u32(b, topo);
  u32(b, 0);
  u32(b, static_cast<std::uint32_t>(idx.size()));
  u32(b, static_cast<std::uint32_t>(sizes.size()));
  for (auto i : idx) u32(b, i);
  for (auto s : sizes) u32(b, s);
  return b;
}
const Bytes kTri3 = floats({0, 0, 0, 1, 0, 0, 0, 1, 0});

bool same_mesh(const PolyMesh& a, const PolyMesh& b) {
  return bit_equal(a.positions, b.positions) && a.face_offsets == b.face_offsets && a.corners == b.corners &&
         a.face_group == b.face_group && a.convention == b.convention && bit_equal(a.attrs.normals, b.attrs.normals) &&
         bit_equal(a.attrs.tangents, b.attrs.tangents) && bit_equal(a.attrs.colors, b.attrs.colors) &&
         a.attrs.uv_dim == b.attrs.uv_dim && a.attrs.uv == b.attrs.uv;
}

}  // namespace

TEST_CASE("rdm: the committed Unity cube reads with volume 1") {
  auto r = read_rdm_file(kCube);
  REQUIRE(r.ok());
  const RdmFile& f = r.value();
  CHECK(f.mesh.convention == Convention::UnityLeftHandedYUpCW);
  CHECK(f.mesh.vertex_count() == 8);
  CHECK(f.mesh.face_count() == 6);
  CHECK(f.mesh.face_group == std::vector<std::uint32_t>(6, 0));
  CHECK(f.mesh.attrs.uv_dim[0] == 2);
  CHECK(f.mesh.attrs.uv[0].size() == 16);
  CHECK(f.warnings.empty());
  CHECK(f.meta_json.find("\"cube\"") != std::string::npos);
  CHECK(signed_volume(f.mesh) == doctest::Approx(1.0));
  CHECK(validate(f.mesh).is_closed_manifold());
  // And after the exact conversion it is still an outward cube.
  CHECK(signed_volume(to_convention(f.mesh, Convention::RightHandedYUpCCW)) == doctest::Approx(1.0));
}

TEST_CASE("rdm: write then read is lossless") {
  PolyMesh m = make_chiral();  // triangles
  m = append(m, make_cube(10, 0, 0));  // plus quads
  m.convention = Convention::UnityLeftHandedYUpCW;
  const std::size_t nv = m.vertex_count();
  m.attrs.normals.assign(nv * 3, 0.5f);
  m.attrs.tangents.assign(nv * 4, -0.25f);
  m.attrs.colors.assign(nv * 4, 0.75f);
  m.attrs.uv[3].assign(nv * 3, 0.125f);
  m.attrs.uv_dim[3] = 3;
  m.attrs.uv[0].assign(nv * 2, 0.5f);
  m.attrs.uv_dim[0] = 2;
  m.positions[1] = -0.0f;  // sign of zero must survive

  RdmWriteOptions o;
  o.extra_meta_json = "{\"label\":\"hello\",\"params\":{\"iterations\":2}}";
  auto bytes = serialize_rdm(m, o);
  REQUIRE(bytes.ok());
  CHECK(bytes.value().size() % 8 == 0);

  auto back = read_rdm(bytes.value().data(), bytes.value().size());
  REQUIRE(back.ok());
  PolyMesh expect = m;
  expect.face_group.assign(m.face_count(), 0);  // the reader always fills face_group
  CHECK(same_mesh(back.value().mesh, expect));
  CHECK(back.value().meta_json.find("\"label\":\"hello\"") != std::string::npos);
  CHECK(back.value().meta_json.find("\"iterations\":2") != std::string::npos);
}

TEST_CASE("rdm: polygons and several groups, including an empty one") {
  // Group 0: a pentagon and a triangle; group 2: a quad. Group 1 is empty and must survive as a submesh.
  PolyMesh m = make_mesh({0, 0, 0, 1, 0, 0, 2, 1, 0, 1, 2, 0, 0, 1, 0, 5, 5, 5, 6, 5, 5, 6, 6, 5, 5, 6, 5},
                         {{0, 1, 2, 3, 4}, {0, 1, 5}, {5, 6, 7, 8}});
  m.face_group = {0, 0, 2};
  auto bytes = serialize_rdm(m);
  REQUIRE(bytes.ok());
  auto back = read_rdm(bytes.value().data(), bytes.value().size());
  REQUIRE(back.ok());
  CHECK(same_mesh(back.value().mesh, m));
  CHECK(back.value().meta_json.find("\"submesh_count\":3") != std::string::npos);
}

TEST_CASE("rdm: faces of one group stay together and keep their relative order") {
  PolyMesh m = make_mesh({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}, {{0, 1, 2}, {1, 3, 2}, {0, 2, 3}});
  m.face_group = {1, 0, 1};
  auto bytes = serialize_rdm(m);
  REQUIRE(bytes.ok());
  auto back = read_rdm(bytes.value().data(), bytes.value().size());
  REQUIRE(back.ok());
  // Written group-wise: group 0 first (face 1), then group 1 (faces 0, 2).
  CHECK(back.value().mesh.corners == std::vector<std::uint32_t>{1, 3, 2, 0, 1, 2, 0, 2, 3});
  CHECK(back.value().mesh.face_group == std::vector<std::uint32_t>{0, 1, 1});
}

TEST_CASE("rdm: the writer refuses meshes the reader would reject") {
  PolyMesh m = make_cube();
  SUBCASE("index out of range") { m.corners[0] = 8; }
  SUBCASE("non-finite position") { m.positions[0] = std::numeric_limits<float>::infinity(); }
  SUBCASE("face with two corners") {
    m.face_offsets = {0, 2};
    m.corners = {0, 1};
  }
  SUBCASE("malformed container") { m.face_offsets.back() = 3; }
  SUBCASE("too many groups") {
    m.face_group.assign(m.face_count(), 0);
    m.face_group[0] = 70000;
  }
  CHECK(serialize_rdm(m).error().code == Errc::InputInvalid);
  CHECK(serialize_rdm(make_cube(), RdmWriteOptions{"x", "[1,2]", ""}).error().code == Errc::ParamInvalid);
}

TEST_CASE("rdm: an empty mesh round-trips") {
  PolyMesh m;
  m.face_offsets = {0};
  auto bytes = serialize_rdm(m);
  REQUIRE(bytes.ok());
  auto back = read_rdm(bytes.value().data(), bytes.value().size());
  REQUIRE(back.ok());
  CHECK(back.value().mesh.vertex_count() == 0);
  CHECK(back.value().mesh.face_count() == 0);
}

TEST_CASE("rdm: every proper prefix of the cube file is rejected without crashing") {
  const Bytes full = slurp(kCube);
  REQUIRE(full.size() > 100);
  REQUIRE(read_rdm(full.data(), full.size()).ok());
  // A file may legitimately lose the last chunk's padding (at most 7 bytes); anything shorter must fail.
  for (std::size_t n = 0; n < full.size(); ++n) {
    auto r = read_rdm(full.data(), n);
    if (n < full.size() - 7) {
      CHECK_MESSAGE(!r.ok(), "prefix length " << n << " was accepted");
    }  // else: ok or error, but never a crash
  }
}

TEST_CASE("rdm: flipping any single byte never crashes and never yields an invalid mesh") {
  const Bytes full = slurp(kCube);
  for (std::size_t i = 0; i < full.size(); ++i) {
    Bytes b = full;
    b[i] ^= 0xFF;
    auto r = read_rdm(b.data(), b.size());
    if (r.ok()) {
      const ValidationReport v = validate(r.value().mesh);
      CHECK_MESSAGE(v.format_errors.empty(), "byte " << i);
      CHECK_MESSAGE(v.index_out_of_range_faces.count == 0, "byte " << i);
      CHECK_MESSAGE(v.nonfinite_vertices.count == 0, "byte " << i);
    }
  }
}

TEST_CASE("rdm: unknown chunks are skipped, including at odd lengths") {
  Bytes f = file({chunk("META", text(meta(3, 1))), chunk("ZZZZ", text("abc")), chunk("VPOS", kTri3),
                  chunk("Q1Q1", Bytes(13, 7)), chunk("SUBM", subm(1, {0, 1, 2}))});
  auto r = read_rdm(f.data(), f.size());
  REQUIRE(r.ok());
  CHECK(r.value().mesh.face_count() == 1);
  CHECK(r.value().mesh.vertex_count() == 3);
}

TEST_CASE("rdm: line and point submeshes are skipped with a warning; group ids still refer to file order") {
  Bytes f = file({chunk("META", text(meta(3, 3))), chunk("VPOS", kTri3), chunk("SUBM", subm(3, {0, 1, 1, 2})),
                  chunk("SUBM", subm(1, {0, 1, 2})), chunk("SUBM", subm(5, {0, 1}))});
  auto r = read_rdm(f.data(), f.size());
  REQUIRE(r.ok());
  CHECK(r.value().mesh.face_count() == 1);
  CHECK(r.value().mesh.face_group == std::vector<std::uint32_t>{1});
  CHECK(r.value().warnings.size() == 2);
}

TEST_CASE("rdm: COMP is carried through verbatim") {
  Bytes f = file({chunk("META", text(meta(3, 0))), chunk("VPOS", kTri3), chunk("COMP", text("[{\"type\":\"X\"}]"))});
  auto r = read_rdm(f.data(), f.size());
  REQUIRE(r.ok());
  CHECK(r.value().components_json == "[{\"type\":\"X\"}]");
}

TEST_CASE("rdm: malformed files are rejected with Errc::Parse") {
  auto parse_fails = [](const Bytes& b) {
    auto r = read_rdm(b.data(), b.size());
    return !r.ok() && r.error().code == Errc::Parse;
  };
  const Bytes ok_tri = floats({0, 0, 0, 1, 0, 0, 0, 1, 0});

  SUBCASE("header") {
    CHECK(parse_fails({}));
    Bytes bad_magic = file({chunk("META", text(meta(3, 0)))});
    bad_magic[0] = 'X';
    CHECK(parse_fails(bad_magic));
    Bytes bad_major = file({chunk("META", text(meta(3, 0))), chunk("VPOS", ok_tri)});
    bad_major[4] = 2;
    CHECK(parse_fails(bad_major));
  }
  SUBCASE("META problems") {
    CHECK(parse_fails(file({chunk("VPOS", ok_tri)})));                                           // META not first
    CHECK(parse_fails(file({chunk("META", text("[1]")), chunk("VPOS", ok_tri)})));              // not an object
    CHECK(parse_fails(file({chunk("META", text("{nope")), chunk("VPOS", ok_tri)})));            // not JSON
    CHECK(parse_fails(file({chunk("META", text("{\"format\":\"rdm\"}")), chunk("VPOS", ok_tri)})));  // missing keys
    CHECK(parse_fails(file({chunk("META", text(meta(3, 0, "martian"))), chunk("VPOS", ok_tri)})));
    CHECK(parse_fails(file({chunk("META", text(meta(3, 0))), chunk("META", text(meta(3, 0))), chunk("VPOS", ok_tri)})));
    CHECK(parse_fails(file({chunk("META", text("{\"format\":\"obj\",\"version\":\"1.0\",\"convention\":\"rh_yup_ccw\","
                                               "\"vertex_count\":3,\"submesh_count\":0}")),
                            chunk("VPOS", ok_tri)})));
    CHECK(parse_fails(file({chunk("META", text("{\"format\":\"rdm\",\"version\":\"1.0\",\"convention\":\"rh_yup_ccw\","
                                               "\"vertex_count\":-3,\"submesh_count\":0}")),
                            chunk("VPOS", ok_tri)})));
    CHECK(parse_fails(file({chunk("META", text(meta(3, 0)))})));  // no VPOS
  }
  SUBCASE("sizes must match vertex_count") {
    CHECK(parse_fails(file({chunk("META", text(meta(4, 0))), chunk("VPOS", ok_tri)})));
    CHECK(parse_fails(file({chunk("META", text(meta(3, 0))), chunk("VPOS", ok_tri), chunk("VNRM", floats({1, 2}))})));
    CHECK(parse_fails(file({chunk("META", text(meta(3, 0))), chunk("VPOS", ok_tri), chunk("UV0 ", floats({1, 2}))})));
    Bytes uv_dim5;
    u32(uv_dim5, 5);
    u32(uv_dim5, 0);
    CHECK(parse_fails(file({chunk("META", text(meta(3, 0))), chunk("VPOS", ok_tri), chunk("UV0 ", uv_dim5)})));
  }
  SUBCASE("submesh problems") {
    auto with = [&](const Bytes& s, std::uint64_t n = 1) {
      return file({chunk("META", text(meta(3, n))), chunk("VPOS", ok_tri), chunk("SUBM", s)});
    };
    CHECK(parse_fails(with(subm(1, {0, 1, 3}))));                // index out of range
    CHECK(parse_fails(with(subm(1, {0, 1}))));                   // not a multiple of 3
    CHECK(parse_fails(with(subm(2, {0, 1, 2}))));                // not a multiple of 4
    CHECK(parse_fails(with(subm(99, {0, 1, 2}))));               // unknown topology
    CHECK(parse_fails(with(subm(16, {0, 1, 2}, {2, 1}))));       // face size < 3
    CHECK(parse_fails(with(subm(16, {0, 1, 2}, {4}))));          // sizes do not sum up
    CHECK(parse_fails(with(subm(1, {0, 1, 2}), 2)));             // submesh_count mismatch
    CHECK(parse_fails(with(Bytes(10, 0))));                      // too short
    Bytes lying = subm(1, {0, 1, 2});
    lying[8] = 100;  // claims 100 indices
    CHECK(parse_fails(with(lying)));
  }
  SUBCASE("non-finite position") {
    CHECK(parse_fails(file({chunk("META", text(meta(3, 0))),
                            chunk("VPOS", floats({0, 0, std::numeric_limits<float>::quiet_NaN(), 1, 0, 0, 0, 1, 0}))})));
  }
}

TEST_CASE("rdm: hostile lengths are rejected before anything is allocated") {
  const Bytes ok_tri = floats({0, 0, 0, 1, 0, 0, 0, 1, 0});
  // vertex_count near 2^64 with a tiny VPOS: must be a parse error, not a huge allocation or an overflow.
  for (std::uint64_t nv : {std::uint64_t{1} << 40, std::uint64_t{1} << 62, std::numeric_limits<std::uint64_t>::max() / 2}) {
    Bytes f = file({chunk("META", text(meta(nv, 0))), chunk("VPOS", ok_tri)});
    CHECK(!read_rdm(f.data(), f.size()).ok());
  }
  // A chunk claiming a payload of 2^64-1 bytes, and one claiming 2^63.
  for (std::uint64_t len : {std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1} << 63}) {
    Bytes f = file({chunk("META", text(meta(3, 0)), len)});
    CHECK(!read_rdm(f.data(), f.size()).ok());
  }
  // chunk_count far larger than the file.
  Bytes f = file({chunk("META", text(meta(3, 0))), chunk("VPOS", ok_tri)});
  const std::uint32_t huge = 0xFFFFFFFFu;
  std::memcpy(f.data() + 12, &huge, 4);
  CHECK(!read_rdm(f.data(), f.size()).ok());
}

TEST_CASE("rdm: files on disk, atomic write, error codes") {
  const auto dir = std::filesystem::temp_directory_path() / "rdesk_test_rdm";
  std::filesystem::create_directories(dir);
  const auto path = dir / "out.rdm";
  std::filesystem::remove(path);

  PolyMesh m = make_cube();
  REQUIRE(write_rdm_file(path, m).ok());
  CHECK(std::filesystem::exists(path));
  CHECK_FALSE(std::filesystem::exists(dir / "out.rdm.tmp"));
  auto back = read_rdm_file(path);
  REQUIRE(back.ok());
  CHECK(back.value().mesh.face_count() == 6);

  CHECK(read_rdm_file(dir / "does_not_exist.rdm").error().code == Errc::IoRead);
  CHECK(write_rdm_file(dir / "no_such_subdir" / "x.rdm", m).error().code == Errc::IoWrite);

  m.corners[0] = 99;
  CHECK(write_rdm_file(path, m).error().code == Errc::InputInvalid);
  CHECK(read_rdm_file(path).ok());  // the failed write left the previous file untouched

  std::filesystem::remove_all(dir);
}
