// Whatever the RDM reader accepts must be a well-formed mesh that the writer accepts and the reader reads back
// bit for bit. Everything else must be rejected with an error, never a crash, hang or huge allocation.
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.hpp"
#include "rdesk/rdm.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;

namespace {
bool same_floats(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  auto first = io::read_rdm(data, size);
  if (!first) {
    FUZZ_CHECK(first.error().code == Errc::Parse || first.error().code == Errc::Internal);
    return 0;
  }
  const PolyMesh& m = first.value().mesh;
  FUZZ_CHECK(format_errors(m).empty());
  const ValidationReport v = validate(m);
  FUZZ_CHECK(!v.has_errors());

  auto bytes = io::serialize_rdm(m);
  if (!bytes) {
    // The only thing the writer may refuse that the reader accepted: more than 65536 submesh groups.
    FUZZ_CHECK(bytes.error().code == Errc::InputInvalid);
    std::uint32_t top = 0;
    for (const std::uint32_t g : m.face_group) top = g > top ? g : top;
    FUZZ_CHECK(top >= 65536);
    return 0;
  }
  auto second = io::read_rdm(bytes.value().data(), bytes.value().size());
  FUZZ_CHECK(second.ok());
  const PolyMesh& n = second.value().mesh;
  FUZZ_CHECK(same_floats(m.positions, n.positions));
  FUZZ_CHECK(m.face_offsets == n.face_offsets);
  FUZZ_CHECK(m.corners == n.corners);
  FUZZ_CHECK(m.face_group == n.face_group);
  FUZZ_CHECK(m.convention == n.convention);
  FUZZ_CHECK(same_floats(m.attrs.normals, n.attrs.normals));
  FUZZ_CHECK(same_floats(m.attrs.tangents, n.attrs.tangents));
  FUZZ_CHECK(same_floats(m.attrs.colors, n.attrs.colors));
  FUZZ_CHECK(m.attrs.uv_dim == n.attrs.uv_dim);
  for (std::size_t i = 0; i < m.attrs.uv.size(); ++i) FUZZ_CHECK(same_floats(m.attrs.uv[i], n.attrs.uv[i]));
  return 0;
}
