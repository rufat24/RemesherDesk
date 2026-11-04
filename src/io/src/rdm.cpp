#include "rdesk/rdm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "rdesk/validate.hpp"

static_assert(std::endian::native == std::endian::little, "RDM is little-endian; add byte swapping for big-endian hosts");

namespace rdesk::io {

namespace {

using json = nlohmann::json;
using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

constexpr size_t kHeaderSize = 16;
constexpr size_t kChunkHeaderSize = 16;
constexpr size_t kSubmHeaderSize = 16;
constexpr uint32_t kTopoTriangles = 1, kTopoQuads = 2, kTopoLines = 3, kTopoLineStrip = 4, kTopoPoints = 5,
                   kTopoPolygons = 16;
constexpr size_t kMaxGroups = 65536;

Error parse_error(std::string msg) { return make_error(Errc::Parse, "RDM: " + std::move(msg)); }

uint32_t rd32(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t rd64(const uint8_t* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
uint32_t rd16(const uint8_t* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

std::string tag_name(const uint8_t* t) {
  std::string s;
  for (int i = 0; i < 4; ++i) s.push_back(t[i] >= 32 && t[i] < 127 ? static_cast<char>(t[i]) : '?');
  return s;
}

bool tag_is(const uint8_t* t, const char (&lit)[5]) noexcept { return std::memcmp(t, lit, 4) == 0; }

// Payload must hold exactly `count` floats.
Result<std::vector<float>> read_floats(const uint8_t* p, uint64_t payload_len, uint64_t count, const char* what) {
  if (count > payload_len / 4 || payload_len != count * 4) {
    return parse_error(std::string(what) + " payload size does not match vertex_count");
  }
  std::vector<float> v(static_cast<size_t>(count));
  if (count != 0) std::memcpy(v.data(), p, static_cast<size_t>(count) * 4);
  return v;
}

struct SubMesh {
  uint32_t topology = 0;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> face_sizes;  // polygons only
};

}  // namespace

Result<RdmFile> read_rdm(const uint8_t* data, size_t size) {
  if (size < kHeaderSize) return parse_error("file too short for header");
  if (std::memcmp(data, "RDMF", 4) != 0) return parse_error("bad magic");
  if (rd16(data + 4) != 1) return parse_error("unsupported major version " + std::to_string(rd16(data + 4)));
  const uint32_t chunk_count = rd32(data + 12);

  RdmFile out;
  PolyMesh& mesh = out.mesh;
  bool have_meta = false, have_vpos = false;
  uint64_t vertex_count = 0, submesh_count = 0;
  std::vector<SubMesh> subs;
  size_t off = kHeaderSize;
  // Floats expected for `stride` per vertex; saturates so a hostile vertex_count cannot overflow.
  auto want = [&](uint64_t stride) {
    return vertex_count > std::numeric_limits<uint64_t>::max() / stride ? std::numeric_limits<uint64_t>::max()
                                                                         : vertex_count * stride;
  };

  for (uint32_t ci = 0; ci < chunk_count; ++ci) {
    if (size - off < kChunkHeaderSize) return parse_error("chunk " + std::to_string(ci) + " header past end of file");
    const uint8_t* tag = data + off;
    const uint64_t plen = rd64(data + off + 8);
    off += kChunkHeaderSize;
    if (plen > size - off) return parse_error("chunk '" + tag_name(tag) + "' payload past end of file");
    const uint8_t* payload = data + off;
    off += static_cast<size_t>(plen);
    // Padding to 8 bytes. A file that ends inside the last chunk's padding is still readable.
    off += std::min<size_t>(static_cast<size_t>((8 - plen % 8) % 8), size - off);

    if (ci == 0 && !tag_is(tag, "META")) return parse_error("first chunk must be META, got '" + tag_name(tag) + "'");

    if (tag_is(tag, "META")) {
      if (have_meta) return parse_error("duplicate META");
      json j = json::parse(payload, payload + plen, nullptr, /*allow_exceptions=*/false);
      if (j.is_discarded() || !j.is_object()) return parse_error("META is not a valid JSON object");
      for (const char* k : {"format", "version", "convention", "vertex_count", "submesh_count"}) {
        if (!j.contains(k)) return parse_error(std::string("META missing key '") + k + "'");
      }
      if (!j["format"].is_string() || j["format"].get<std::string>() != "rdm") return parse_error("META.format != 'rdm'");
      if (!j["convention"].is_string()) return parse_error("META.convention is not a string");
      const std::string conv = j["convention"].get<std::string>();
      if (conv == "unity_lh_yup_cw") {
        mesh.convention = Convention::UnityLeftHandedYUpCW;
      } else if (conv == "rh_yup_ccw") {
        mesh.convention = Convention::RightHandedYUpCCW;
      } else {
        return parse_error("unknown convention '" + conv + "'");
      }
      if (!j["vertex_count"].is_number_unsigned() || !j["submesh_count"].is_number_unsigned()) {
        return parse_error("META vertex_count / submesh_count must be non-negative integers");
      }
      vertex_count = j["vertex_count"].get<uint64_t>();
      submesh_count = j["submesh_count"].get<uint64_t>();
      out.meta_json = j.dump();
      have_meta = true;
    } else if (tag_is(tag, "VPOS")) {
      auto r = read_floats(payload, plen, want(3), "VPOS");
      if (!r) return r.error();
      mesh.positions = std::move(r).value();
      have_vpos = true;
    } else if (tag_is(tag, "VNRM") || tag_is(tag, "VTAN") || tag_is(tag, "VCOL")) {
      const uint64_t stride = tag_is(tag, "VNRM") ? 3 : 4;
      auto r = read_floats(payload, plen, want(stride), tag_name(tag).c_str());
      if (!r) return r.error();
      (tag_is(tag, "VNRM") ? mesh.attrs.normals : tag_is(tag, "VTAN") ? mesh.attrs.tangents : mesh.attrs.colors) =
          std::move(r).value();
    } else if (tag[0] == 'U' && tag[1] == 'V' && tag[2] >= '0' && tag[2] <= '7' && tag[3] == ' ') {
      if (plen < 8) return parse_error("UV chunk too short");
      const uint32_t dim = rd32(payload);
      if (dim < 2 || dim > 4) return parse_error("UV dimension " + std::to_string(dim) + " invalid");
      auto r = read_floats(payload + 8, plen - 8, want(dim), "UV");
      if (!r) return r.error();
      const size_t ch = static_cast<size_t>(tag[2] - '0');
      mesh.attrs.uv[ch] = std::move(r).value();
      mesh.attrs.uv_dim[ch] = static_cast<uint8_t>(dim);
    } else if (tag_is(tag, "SUBM")) {
      if (!have_meta) return parse_error("SUBM before META");
      if (plen < kSubmHeaderSize) return parse_error("SUBM too short");
      SubMesh sm;
      sm.topology = rd32(payload);
      const uint64_t icount = rd32(payload + 8), fcount = rd32(payload + 12);
      const bool poly = sm.topology == kTopoPolygons;
      const uint64_t need = kSubmHeaderSize + 4 * icount + (poly ? 4 * fcount : 0);
      if (plen != need) return parse_error("SUBM size " + std::to_string(plen) + " != expected " + std::to_string(need));
      switch (sm.topology) {
        case kTopoTriangles: case kTopoQuads: case kTopoLines: case kTopoLineStrip: case kTopoPoints: case kTopoPolygons: break;
        default: return parse_error("unknown topology " + std::to_string(sm.topology));
      }
      const uint64_t per = sm.topology == kTopoTriangles ? 3 : sm.topology == kTopoQuads ? 4 : sm.topology == kTopoLines ? 2 : 1;
      if (icount % per != 0) return parse_error("SUBM index count is not divisible by the face size");
      sm.indices.resize(static_cast<size_t>(icount));
      if (icount) std::memcpy(sm.indices.data(), payload + kSubmHeaderSize, static_cast<size_t>(icount) * 4);
      for (const uint32_t i : sm.indices) {
        if (i >= vertex_count) return parse_error("index " + std::to_string(i) + " out of range");
      }
      if (poly) {
        sm.face_sizes.resize(static_cast<size_t>(fcount));
        if (fcount) std::memcpy(sm.face_sizes.data(), payload + kSubmHeaderSize + 4 * icount, static_cast<size_t>(fcount) * 4);
        uint64_t sum = 0;
        for (const uint32_t s : sm.face_sizes) {
          if (s < 3) return parse_error("polygon face size < 3");
          sum += s;
        }
        if (sum != icount) return parse_error("polygon face sizes do not sum to the index count");
      }
      subs.push_back(std::move(sm));
    } else if (tag_is(tag, "COMP")) {
      out.components_json.assign(reinterpret_cast<const char*>(payload), static_cast<size_t>(plen));
    }
    // Unknown tags: skipped.
  }

  if (!have_meta) return parse_error("no META chunk");
  if (!have_vpos) return parse_error("no VPOS chunk");
  if (subs.size() != submesh_count) {
    return parse_error("submesh_count " + std::to_string(submesh_count) + " != SUBM chunks " + std::to_string(subs.size()));
  }
  for (const float x : mesh.positions) {
    if (!std::isfinite(x)) return parse_error("non-finite vertex position");
  }

  // Flatten into CSR.
  uint64_t total_corners = 0;
  for (const SubMesh& s : subs) {
    if (s.topology == kTopoTriangles || s.topology == kTopoQuads || s.topology == kTopoPolygons) total_corners += s.indices.size();
  }
  if (total_corners >= std::numeric_limits<uint32_t>::max()) return parse_error("more than 2^32-2 corners");
  mesh.face_offsets.push_back(0);
  for (size_t si = 0; si < subs.size(); ++si) {
    const SubMesh& s = subs[si];
    if (s.topology == kTopoLines || s.topology == kTopoLineStrip || s.topology == kTopoPoints) {
      out.warnings.push_back("submesh " + std::to_string(si) + " has line/point topology and was skipped");
      continue;
    }
    size_t pos = 0;
    auto add_face = [&](size_t n) {
      mesh.corners.insert(mesh.corners.end(), s.indices.begin() + static_cast<std::ptrdiff_t>(pos),
                          s.indices.begin() + static_cast<std::ptrdiff_t>(pos + n));
      mesh.face_offsets.push_back(static_cast<uint32_t>(mesh.corners.size()));
      mesh.face_group.push_back(static_cast<uint32_t>(si));
      pos += n;
    };
    if (s.topology == kTopoPolygons) {
      for (const uint32_t n : s.face_sizes) add_face(n);
    } else {
      const size_t n = s.topology == kTopoTriangles ? 3 : 4;
      while (pos < s.indices.size()) add_face(n);
    }
  }
  return out;
}

Result<RdmFile> read_rdm_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return make_error(Errc::IoRead, "cannot open " + path.string());
  in.seekg(0, std::ios::end);
  const auto len = in.tellg();
  if (len < 0) return make_error(Errc::IoRead, "cannot determine size of " + path.string());
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(len));
  if (len > 0 && !in.read(reinterpret_cast<char*>(buf.data()), len)) {
    return make_error(Errc::IoRead, "read error on " + path.string());
  }
  return read_rdm(buf.data(), buf.size());
}

// ------------------------------------------------------------------------------------------ writing

namespace {

void put32(std::vector<uint8_t>& b, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 4);
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 8);
}
void put_floats(std::vector<uint8_t>& b, const std::vector<float>& f) {
  const auto* p = reinterpret_cast<const uint8_t*>(f.data());
  b.insert(b.end(), p, p + f.size() * 4);
}

// A chunk under construction: writes the header, the caller appends the payload, finish() patches the length
// and pads.
class ChunkWriter {
 public:
  ChunkWriter(std::vector<uint8_t>& out, const char (&tag)[5]) : out_(out) {
    out_.insert(out_.end(), tag, tag + 4);
    put32(out_, 0);
    len_pos_ = out_.size();
    put64(out_, 0);
    start_ = out_.size();
  }
  std::vector<uint8_t>& buf() { return out_; }
  void finish() {
    const uint64_t len = out_.size() - start_;
    std::memcpy(out_.data() + len_pos_, &len, 8);
    out_.insert(out_.end(), (8 - len % 8) % 8, uint8_t{0});
  }

 private:
  std::vector<uint8_t>& out_;
  size_t len_pos_ = 0, start_ = 0;
};

}  // namespace

Result<std::vector<uint8_t>> serialize_rdm(const PolyMesh& mesh, const RdmWriteOptions& opts) {
  if (auto fe = format_errors(mesh); !fe.empty()) return make_error(Errc::InputInvalid, "RDM: cannot write a malformed mesh: " + fe.front());
  const size_t nv = mesh.vertex_count();
  for (const uint32_t c : mesh.corners) {
    if (c >= nv) return make_error(Errc::InputInvalid, "RDM: cannot write: corner index out of range");
  }
  for (const float x : mesh.positions) {
    if (!std::isfinite(x)) return make_error(Errc::InputInvalid, "RDM: cannot write: non-finite vertex position");
  }
  const size_t nf = mesh.face_count();
  for (size_t f = 0; f < nf; ++f) {
    if (mesh.face_size(f) < 3) return make_error(Errc::InputInvalid, "RDM: cannot write: face with fewer than 3 corners");
  }

  size_t groups = 1;
  if (!mesh.face_group.empty()) {
    groups = static_cast<size_t>(*std::max_element(mesh.face_group.begin(), mesh.face_group.end())) + 1;
    if (groups > kMaxGroups) return make_error(Errc::InputInvalid, "RDM: cannot write: more than 65536 face groups");
  }
  std::vector<std::vector<uint32_t>> faces_of(groups);  // faces per group, in original order
  for (size_t f = 0; f < nf; ++f) faces_of[mesh.face_group.empty() ? 0 : mesh.face_group[f]].push_back(static_cast<uint32_t>(f));

  json meta = json::object();
  if (!opts.extra_meta_json.empty()) {
    json extra = json::parse(opts.extra_meta_json, nullptr, false);
    if (extra.is_discarded() || !extra.is_object()) return make_error(Errc::ParamInvalid, "RDM: extra_meta_json is not a JSON object");
    meta = std::move(extra);
  }
  meta["format"] = "rdm";
  meta["version"] = "1.0";
  meta["convention"] = to_string(mesh.convention);
  meta["vertex_count"] = nv;
  meta["submesh_count"] = groups;
  meta["producer"] = opts.producer;

  std::vector<uint8_t> out;
  out.reserve(64 + nv * 12 + mesh.corners.size() * 4 + nf * 4);
  const char magic[4] = {'R', 'D', 'M', 'F'};
  out.insert(out.end(), magic, magic + 4);
  const std::uint16_t major = 1, minor = 0;
  out.insert(out.end(), reinterpret_cast<const uint8_t*>(&major), reinterpret_cast<const uint8_t*>(&major) + 2);
  out.insert(out.end(), reinterpret_cast<const uint8_t*>(&minor), reinterpret_cast<const uint8_t*>(&minor) + 2);
  put32(out, 0);
  const size_t count_pos = out.size();
  put32(out, 0);
  uint32_t chunks = 0;

  {
    ChunkWriter c(out, "META");
    const std::string text = meta.dump();
    c.buf().insert(c.buf().end(), text.begin(), text.end());
    c.finish();
    ++chunks;
  }
  auto floats_chunk = [&](const char (&tag)[5], const std::vector<float>& v) {
    if (v.empty() && nv != 0) return;
    ChunkWriter c(out, tag);
    put_floats(c.buf(), v);
    c.finish();
    ++chunks;
  };
  floats_chunk("VPOS", mesh.positions);
  if (!mesh.attrs.normals.empty()) floats_chunk("VNRM", mesh.attrs.normals);
  if (!mesh.attrs.tangents.empty()) floats_chunk("VTAN", mesh.attrs.tangents);
  if (!mesh.attrs.colors.empty()) floats_chunk("VCOL", mesh.attrs.colors);
  for (size_t i = 0; i < mesh.attrs.uv.size(); ++i) {
    if (mesh.attrs.uv[i].empty()) continue;
    const char tag[5] = {'U', 'V', static_cast<char>('0' + i), ' ', 0};
    ChunkWriter c(out, tag);
    put32(c.buf(), mesh.attrs.uv_dim[i]);
    put32(c.buf(), 0);
    put_floats(c.buf(), mesh.attrs.uv[i]);
    c.finish();
    ++chunks;
  }

  for (size_t g = 0; g < groups; ++g) {
    const auto& fl = faces_of[g];
    bool all3 = true, all4 = true;
    uint64_t icount = 0;
    for (const uint32_t f : fl) {
      const size_t n = mesh.face_size(f);
      all3 = all3 && n == 3;
      all4 = all4 && n == 4;
      icount += n;
    }
    const uint32_t topo = fl.empty() || all3 ? kTopoTriangles : all4 ? kTopoQuads : kTopoPolygons;
    ChunkWriter c(out, "SUBM");
    put32(c.buf(), topo);
    put32(c.buf(), 0);  // base_vertex: indices are absolute
    put32(c.buf(), static_cast<uint32_t>(icount));
    put32(c.buf(), topo == kTopoPolygons ? static_cast<uint32_t>(fl.size()) : 0);
    for (const uint32_t f : fl) {
      for (uint32_t k = mesh.face_offsets[f]; k < mesh.face_offsets[f + 1]; ++k) put32(c.buf(), mesh.corners[k]);
    }
    if (topo == kTopoPolygons) {
      for (const uint32_t f : fl) put32(c.buf(), static_cast<uint32_t>(mesh.face_size(f)));
    }
    c.finish();
    ++chunks;
  }

  if (!opts.components_json.empty()) {
    ChunkWriter c(out, "COMP");
    c.buf().insert(c.buf().end(), opts.components_json.begin(), opts.components_json.end());
    c.finish();
    ++chunks;
  }
  std::memcpy(out.data() + count_pos, &chunks, 4);
  return out;
}

Result<void> write_rdm_file(const std::filesystem::path& path, const PolyMesh& mesh, const RdmWriteOptions& opts) {
  auto bytes = serialize_rdm(mesh, opts);
  if (!bytes) return bytes.error();
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return make_error(Errc::IoWrite, "cannot create " + tmp.string());
    o.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
    o.flush();
    if (!o) return make_error(Errc::IoWrite, "write error on " + tmp.string());
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return make_error(Errc::IoWrite, "cannot rename onto " + path.string());
  }
  return {};
}

}  // namespace rdesk::io
