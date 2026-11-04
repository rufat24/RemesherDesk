#include "rdesk/obj.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <fstream>
#include <map>
#include <system_error>
#include <unordered_set>

#include "rdesk/convention.hpp"
#include "rdesk/validate.hpp"
#include "tiny_obj_loader.h"

namespace rdesk::io {

namespace {

using std::size_t;
using std::uint32_t;

Error parse_error(std::string msg) { return make_error(Errc::Parse, "OBJ: " + std::move(msg)); }

// tinyobjloader maps `usemtl` names to ids through the .mtl it is given; with no .mtl every usemtl gets
// id -1 and different materials could not be told apart. So we synthesise a minimal .mtl that merely
// declares each name. (This function only sees names; it never reads the file system.)
std::string synthesize_mtl(std::string_view text) {
  std::string mtl;
  std::unordered_set<std::string> seen;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    std::string_view line = text.substr(pos, eol - pos);
    pos = eol + 1;
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string_view::npos || line.compare(b, 6, "usemtl") != 0) continue;
    b += 6;
    if (b >= line.size() || (line[b] != ' ' && line[b] != '\t')) continue;
    const size_t s = line.find_first_not_of(" \t", b);
    if (s == std::string_view::npos) continue;
    size_t e = line.find_last_not_of(" \t\r");
    const std::string name(line.substr(s, e - s + 1));
    if (seen.insert(name).second) mtl += "newmtl " + name + "\n";
  }
  return mtl;
}

struct Tuple {  // (v, vt, vn); -1 = absent
  int v, t, n;
  bool operator<(const Tuple& o) const noexcept {
    if (v != o.v) return v < o.v;
    if (t != o.t) return t < o.t;
    return n < o.n;
  }
  bool operator==(const Tuple& o) const noexcept { return v == o.v && t == o.t && n == o.n; }
};

Result<ObjFile> read_obj_impl(std::string_view text, const ObjReadOptions& opts) {
  tinyobj::ObjReaderConfig cfg;
  cfg.triangulate = false;
  cfg.vertex_color = false;
  tinyobj::ObjReader reader;
  // The supplied .mtl text is only consulted when the OBJ contains an `mtllib` line (the file name is ignored),
  // so prepend one to our private copy whenever there is something to declare. A file's own `mtllib` lines
  // then find the stream already consumed, which is harmless.
  const std::string mtl = synthesize_mtl(text);
  std::string source;
  source.reserve(text.size() + 32);
  if (!mtl.empty()) source += "mtllib rdesk-synthesized.mtl\n";
  source.append(text);
  if (!reader.ParseFromString(source, mtl, cfg)) {
    std::string why = reader.Error();
    while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
    return parse_error(why.empty() ? "tinyobjloader rejected the file" : why);
  }

  ObjFile out;
  if (!reader.Warning().empty()) {
    std::string w = reader.Warning();
    while (!w.empty() && (w.back() == '\n' || w.back() == '\r')) w.pop_back();
    out.warnings.push_back(std::move(w));
  }
  const tinyobj::attrib_t& A = reader.GetAttrib();
  const size_t nv = A.vertices.size() / 3, nn = A.normals.size() / 3, nt = A.texcoords.size() / 2;
  if (nv >= static_cast<size_t>(INT32_MAX)) return parse_error("too many vertices");

  // ---- faces, in file order, with their (shape name, material) group
  std::vector<Tuple> corner_tuples;
  std::vector<uint32_t> offsets{0};
  std::vector<uint32_t> groups;
  std::map<std::pair<std::string, int>, uint32_t> group_ids;
  size_t skipped = 0;
  bool has_vt = false, has_vn = false;
  for (const tinyobj::shape_t& sh : reader.GetShapes()) {
    const auto& m = sh.mesh;
    size_t ci = 0;
    for (size_t f = 0; f < m.num_face_vertices.size(); ++f) {
      const size_t n = m.num_face_vertices[f];
      if (ci + n > m.indices.size()) return parse_error("inconsistent face data");
      if (n < 3) {
        ++skipped;
        ci += n;
        continue;
      }
      const int mat = f < m.material_ids.size() ? m.material_ids[f] : -1;
      auto [it, inserted] = group_ids.try_emplace({sh.name, mat}, static_cast<uint32_t>(group_ids.size()));
      if (inserted) {
        std::string label = sh.name;
        if (mat >= 0 && static_cast<size_t>(mat) < reader.GetMaterials().size()) {
          label += "|" + reader.GetMaterials()[static_cast<size_t>(mat)].name;
        }
        out.group_labels.push_back(std::move(label));
      }
      for (size_t k = 0; k < n; ++k) {
        const tinyobj::index_t& ix = m.indices[ci + k];
        if (ix.vertex_index < 0 || static_cast<size_t>(ix.vertex_index) >= nv) return parse_error("vertex index out of range");
        if (ix.normal_index < -1 || (ix.normal_index >= 0 && static_cast<size_t>(ix.normal_index) >= nn)) {
          return parse_error("normal index out of range");
        }
        if (ix.texcoord_index < -1 || (ix.texcoord_index >= 0 && static_cast<size_t>(ix.texcoord_index) >= nt)) {
          return parse_error("texcoord index out of range");
        }
        has_vt = has_vt || ix.texcoord_index >= 0;
        has_vn = has_vn || ix.normal_index >= 0;
        corner_tuples.push_back({ix.vertex_index, ix.texcoord_index < 0 ? -1 : ix.texcoord_index,
                                 ix.normal_index < 0 ? -1 : ix.normal_index});
      }
      ci += n;
      if (corner_tuples.size() >= UINT32_MAX) return parse_error("more than 2^32-2 corners");
      offsets.push_back(static_cast<uint32_t>(corner_tuples.size()));
      groups.push_back(it->second);
    }
  }
  if (skipped) out.warnings.push_back(std::to_string(skipped) + " face(s) with fewer than 3 vertices were skipped");

  PolyMesh& mesh = out.mesh;
  mesh.convention = opts.source_convention;
  mesh.face_offsets = std::move(offsets);
  mesh.face_group = std::move(groups);
  mesh.corners.resize(corner_tuples.size());

  if (!has_vt && !has_vn) {
    // Fast path: vertex numbering is the `v` numbering.
    mesh.positions.assign(A.vertices.begin(), A.vertices.end());
    for (size_t c = 0; c < corner_tuples.size(); ++c) mesh.corners[c] = static_cast<uint32_t>(corner_tuples[c].v);
    return out;
  }

  // General path: unique (v, vt, vn) tuples in ascending order; unreferenced `v` entries stay as vertices.
  std::vector<Tuple> uniq = corner_tuples;
  std::sort(uniq.begin(), uniq.end());
  uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
  {
    std::vector<uint8_t> used(nv, 0);
    for (const Tuple& t : uniq) used[static_cast<size_t>(t.v)] = 1;
    for (size_t v = 0; v < nv; ++v) {
      if (!used[v]) uniq.push_back({static_cast<int>(v), -1, -1});
    }
    std::sort(uniq.begin(), uniq.end());
  }
  if (uniq.size() >= UINT32_MAX) return parse_error("too many vertices");
  for (size_t c = 0; c < corner_tuples.size(); ++c) {
    mesh.corners[c] = static_cast<uint32_t>(std::lower_bound(uniq.begin(), uniq.end(), corner_tuples[c]) - uniq.begin());
  }
  mesh.positions.reserve(uniq.size() * 3);
  if (has_vn) mesh.attrs.normals.reserve(uniq.size() * 3);
  if (has_vt) mesh.attrs.uv[0].reserve(uniq.size() * 2);
  size_t missing_vt = 0, missing_vn = 0;
  for (const Tuple& t : uniq) {
    const size_t v = static_cast<size_t>(t.v);
    mesh.positions.insert(mesh.positions.end(), A.vertices.begin() + static_cast<std::ptrdiff_t>(3 * v),
                          A.vertices.begin() + static_cast<std::ptrdiff_t>(3 * v + 3));
    if (has_vn) {
      if (t.n >= 0) {
        const size_t n = static_cast<size_t>(t.n);
        mesh.attrs.normals.insert(mesh.attrs.normals.end(), A.normals.begin() + static_cast<std::ptrdiff_t>(3 * n),
                                  A.normals.begin() + static_cast<std::ptrdiff_t>(3 * n + 3));
      } else {
        mesh.attrs.normals.insert(mesh.attrs.normals.end(), 3, 0.0f);
        ++missing_vn;
      }
    }
    if (has_vt) {
      if (t.t >= 0) {
        const size_t u = static_cast<size_t>(t.t);
        mesh.attrs.uv[0].insert(mesh.attrs.uv[0].end(), A.texcoords.begin() + static_cast<std::ptrdiff_t>(2 * u),
                                A.texcoords.begin() + static_cast<std::ptrdiff_t>(2 * u + 2));
      } else {
        mesh.attrs.uv[0].insert(mesh.attrs.uv[0].end(), 2, 0.0f);
        ++missing_vt;
      }
    }
  }
  if (has_vt) mesh.attrs.uv_dim[0] = 2;
  if (missing_vn) out.warnings.push_back(std::to_string(missing_vn) + " vertex(es) have no normal; zeros were used");
  if (missing_vt) out.warnings.push_back(std::to_string(missing_vt) + " vertex(es) have no texture coordinate; zeros were used");
  return out;
}

}  // namespace

Result<ObjFile> read_obj(std::string_view text, const ObjReadOptions& opts) {
  try {
    return read_obj_impl(text, opts);
  } catch (const std::exception& e) {  // bad_alloc, length_error from huge inputs
    return make_error(Errc::Internal, std::string("OBJ: unexpected failure: ") + e.what());
  }
}

Result<ObjFile> read_obj_file(const std::filesystem::path& path, const ObjReadOptions& opts) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return make_error(Errc::IoRead, "cannot open " + path.string());
  in.seekg(0, std::ios::end);
  const auto len = in.tellg();
  if (len < 0) return make_error(Errc::IoRead, "cannot determine size of " + path.string());
  in.seekg(0, std::ios::beg);
  std::string buf;
  try {
    buf.resize(static_cast<size_t>(len));
  } catch (const std::exception&) {
    return make_error(Errc::IoRead, "file too large to load: " + path.string());
  }
  if (len > 0 && !in.read(buf.data(), len)) return make_error(Errc::IoRead, "read error on " + path.string());
  return read_obj(buf, opts);
}

// ------------------------------------------------------------------------------------------ writing

namespace {

void append_float(std::string& s, float v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  s.append(buf, r.ptr);
}
void append_uint(std::string& s, uint32_t v) {
  char buf[16];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  s.append(buf, r.ptr);
}

bool all_finite(const std::vector<float>& v) {
  return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}

}  // namespace

Result<std::string> serialize_obj(const PolyMesh& mesh_in, const ObjWriteOptions& opts) {
  try {
    if (auto fe = format_errors(mesh_in); !fe.empty()) {
      return make_error(Errc::InputInvalid, "OBJ: cannot write a malformed mesh: " + fe.front());
    }
    const size_t nv = mesh_in.vertex_count();
    for (const uint32_t c : mesh_in.corners) {
      if (c >= nv) return make_error(Errc::InputInvalid, "OBJ: cannot write: corner index out of range");
    }
    if (!all_finite(mesh_in.positions) || !all_finite(mesh_in.attrs.normals) || !all_finite(mesh_in.attrs.uv[0])) {
      return make_error(Errc::InputInvalid, "OBJ: cannot write: non-finite position or attribute");
    }
    const PolyMesh mesh = to_convention(mesh_in, opts.target);
    const bool with_vn = !mesh.attrs.normals.empty();
    const bool with_vt = !mesh.attrs.uv[0].empty() && mesh.attrs.uv_dim[0] >= 2;
    const size_t uv_stride = mesh.attrs.uv_dim[0];

    std::string out;
    out.reserve(64 + nv * 40 + mesh.corners.size() * 12);
    out += "# RemesherDesk OBJ, convention ";
    out += to_string(opts.target);
    out += "\n";
    {  // user comment, one "# " per line
      size_t pos = 0;
      while (pos < opts.comment.size()) {
        size_t eol = opts.comment.find('\n', pos);
        if (eol == std::string::npos) eol = opts.comment.size();
        out += "# ";
        out.append(opts.comment, pos, eol - pos);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        out += "\n";
        pos = eol + 1;
      }
    }
    for (size_t v = 0; v < nv; ++v) {
      out += "v ";
      append_float(out, mesh.positions[3 * v]);
      out += ' ';
      append_float(out, mesh.positions[3 * v + 1]);
      out += ' ';
      append_float(out, mesh.positions[3 * v + 2]);
      out += '\n';
    }
    if (with_vt) {
      for (size_t v = 0; v < nv; ++v) {
        out += "vt ";
        append_float(out, mesh.attrs.uv[0][uv_stride * v]);
        out += ' ';
        append_float(out, mesh.attrs.uv[0][uv_stride * v + 1]);
        out += '\n';
      }
    }
    if (with_vn) {
      for (size_t v = 0; v < nv; ++v) {
        out += "vn ";
        append_float(out, mesh.attrs.normals[3 * v]);
        out += ' ';
        append_float(out, mesh.attrs.normals[3 * v + 1]);
        out += ' ';
        append_float(out, mesh.attrs.normals[3 * v + 2]);
        out += '\n';
      }
    }

    const size_t nf = mesh.face_count();
    std::vector<uint32_t> order(nf);
    for (size_t f = 0; f < nf; ++f) order[f] = static_cast<uint32_t>(f);
    if (!mesh.face_group.empty()) {
      std::stable_sort(order.begin(), order.end(),
                       [&](uint32_t a, uint32_t b) { return mesh.face_group[a] < mesh.face_group[b]; });
    }
    uint32_t current = UINT32_MAX;
    for (const uint32_t f : order) {
      if (!mesh.face_group.empty() && mesh.face_group[f] != current) {
        current = mesh.face_group[f];
        out += "g ";
        if (opts.group_labels && current < opts.group_labels->size() && !(*opts.group_labels)[current].empty()) {
          out += (*opts.group_labels)[current];
        } else {
          out += "group_" + std::to_string(current);
        }
        out += '\n';
      }
      out += 'f';
      for (uint32_t k = mesh.face_offsets[f]; k < mesh.face_offsets[f + 1]; ++k) {
        const uint32_t i = mesh.corners[k] + 1;
        out += ' ';
        append_uint(out, i);
        if (with_vt || with_vn) {
          out += '/';
          if (with_vt) append_uint(out, i);
          if (with_vn) {
            out += '/';
            append_uint(out, i);
          }
        }
      }
      out += '\n';
    }
    return out;
  } catch (const std::exception& e) {
    return make_error(Errc::Internal, std::string("OBJ: unexpected failure: ") + e.what());
  }
}

Result<void> write_obj_file(const std::filesystem::path& path, const PolyMesh& mesh, const ObjWriteOptions& opts) {
  auto text = serialize_obj(mesh, opts);
  if (!text) return text.error();
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return make_error(Errc::IoWrite, "cannot create " + tmp.string());
    o.write(text.value().data(), static_cast<std::streamsize>(text.value().size()));
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
