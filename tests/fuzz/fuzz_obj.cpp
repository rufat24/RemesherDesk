// Whatever the OBJ reader accepts must be a well-formed mesh. If it is finite, the writer must accept it and a
// second read must reproduce the positions bit for bit and the face count (and the faces themselves when the
// groups are already in ascending order, which is the order the writer uses).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "check.hpp"
#include "rdesk/obj.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  auto first = io::read_obj(std::string_view(reinterpret_cast<const char*>(data), size));
  if (!first) {
    FUZZ_CHECK(first.error().code == Errc::Parse || first.error().code == Errc::Internal);
    return 0;
  }
  const PolyMesh& m = first.value().mesh;
  FUZZ_CHECK(format_errors(m).empty());
  for (const std::uint32_t c : m.corners) FUZZ_CHECK(c < m.vertex_count());
  FUZZ_CHECK(m.face_group.size() == m.face_count());
  for (const std::uint32_t g : m.face_group) FUZZ_CHECK(g < first.value().group_labels.size());

  bool finite = true;
  for (const float x : m.positions) finite = finite && std::isfinite(x);
  for (const float x : m.attrs.normals) finite = finite && std::isfinite(x);
  for (const float x : m.attrs.uv[0]) finite = finite && std::isfinite(x);

  auto text = io::serialize_obj(m);
  if (!finite) {
    FUZZ_CHECK(!text.ok() && text.error().code == Errc::InputInvalid);
    return 0;
  }
  FUZZ_CHECK(text.ok());
  auto second = io::read_obj(text.value());
  FUZZ_CHECK(second.ok());
  const PolyMesh& n = second.value().mesh;
  FUZZ_CHECK(n.positions.size() == m.positions.size());
  FUZZ_CHECK(m.positions.empty() || std::memcmp(n.positions.data(), m.positions.data(), m.positions.size() * sizeof(float)) == 0);
  FUZZ_CHECK(n.face_count() == m.face_count());
  bool ascending = true;
  for (std::size_t i = 1; i < m.face_group.size(); ++i) ascending = ascending && m.face_group[i - 1] <= m.face_group[i];
  if (ascending) {
    FUZZ_CHECK(n.face_offsets == m.face_offsets);
    FUZZ_CHECK(n.corners == m.corners);
  }
  return 0;
}
