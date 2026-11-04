// Writes the canonical Unity-convention cube (the one tools/rdm/rdm.py calls unity_cube()) with the C++ writer,
// so ctest can ask the Python reference reader to verify it.
#include <cstdio>

#include "rdesk/rdm.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: rdm_write_cube OUT.rdm\n");
    return 2;
  }
  rdesk::PolyMesh m;
  m.convention = rdesk::Convention::UnityLeftHandedYUpCW;
  m.positions = {-.5f, -.5f, -.5f, .5f, -.5f, -.5f, .5f, .5f, -.5f, -.5f, .5f, -.5f,
                 -.5f, -.5f, .5f,  .5f, -.5f, .5f,  .5f, .5f, .5f,  -.5f, .5f, .5f};
  m.face_offsets = {0, 4, 8, 12, 16, 20, 24};
  m.corners = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5};
  m.attrs.uv[0] = {0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f};
  m.attrs.uv_dim[0] = 2;
  rdesk::io::RdmWriteOptions opts;
  opts.producer = "tests/tools/rdm_write_cube.cpp";
  const auto r = rdesk::io::write_rdm_file(argv[1], m, opts);
  if (!r) {
    std::fprintf(stderr, "%s\n", r.error().message.c_str());
    return 1;
  }
  return 0;
}
