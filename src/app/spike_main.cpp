// GUI spike (task A10): proves the Polyscope stack for decision D3. Loads a mesh, shows it with its polygons
// (quads stay quads), wireframe on, a per-face "corners" colouring, and an ImGui panel of our own in place of
// Polyscope's default panels. `--screenshot FILE` renders one frame to a PNG and exits (used by tests).
// The real application (A11) replaces this file.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "imgui.h"
#include "polyscope/polyscope.h"
#include "polyscope/screenshot.h"
#include "polyscope/surface_mesh.h"
#include "rdesk/convention.hpp"
#include "rdesk/obj.hpp"
#include "rdesk/paths.hpp"
#include "rdesk/rdm.hpp"
#include "rdesk/validate.hpp"

namespace {

struct Options {
  std::string mesh, screenshot;
  int width = 1280, height = 800;
};

rdesk::Result<rdesk::PolyMesh> load(const std::string& path) {
  const auto p = rdesk::platform::path_from_utf8(path);
  const std::string ext = rdesk::platform::path_to_utf8(p.extension());
  if (ext == ".obj") {
    auto r = rdesk::io::read_obj_file(p);
    if (!r) return r.error();
    return std::move(r.value().mesh);
  }
  if (ext == ".rdm") {
    auto r = rdesk::io::read_rdm_file(p);
    if (!r) return r.error();
    return std::move(r.value().mesh);
  }
  return rdesk::make_error(rdesk::Errc::Parse, "expected a .obj or .rdm file");
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
    else if (a == "--size" && i + 2 < argc) { o.width = std::atoi(argv[i + 1]); o.height = std::atoi(argv[i + 2]); i += 2; }
    else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    else o.mesh = a;
  }
  if (o.mesh.empty()) {
    std::fprintf(stderr, "usage: RemesherDesk [--screenshot out.png] [--size W H] mesh.(obj|rdm)\n");
    return 2;
  }
  auto loaded = load(o.mesh);
  if (!loaded) {
    std::fprintf(stderr, "error: %s\n", loaded.error().message.c_str());
    return 3;
  }
  rdesk::PolyMesh mesh = rdesk::to_convention(loaded.value(), rdesk::Convention::RightHandedYUpCCW);

  std::vector<std::array<double, 3>> vertices(mesh.vertex_count());
  for (std::size_t v = 0; v < vertices.size(); ++v) vertices[v] = {mesh.positions[3 * v], mesh.positions[3 * v + 1], mesh.positions[3 * v + 2]};
  std::vector<std::vector<std::size_t>> faces(mesh.face_count());
  std::vector<float> corners(mesh.face_count());
  std::size_t tri = 0, quad = 0, other = 0;
  for (std::size_t f = 0; f < faces.size(); ++f) {
    faces[f].assign(mesh.corners.begin() + mesh.face_offsets[f], mesh.corners.begin() + mesh.face_offsets[f + 1]);
    corners[f] = static_cast<float>(faces[f].size());
    (faces[f].size() == 3 ? tri : faces[f].size() == 4 ? quad : other)++;
  }

  polyscope::options::programName = "RemesherDesk";
  polyscope::options::buildDefaultGuiPanels = false;           // our own panels instead (decision D3, criterion 3)
  polyscope::options::openImGuiWindowForUserCallback = false;  // no empty "user gui" window: the callback opens its own
  polyscope::options::usePrefsFile = false;
  polyscope::options::alwaysRedraw = false;
  polyscope::options::groundPlaneMode = polyscope::GroundPlaneMode::None;
  polyscope::view::setWindowSize(o.width, o.height);
  polyscope::init();

  auto* ps = polyscope::registerSurfaceMesh("mesh", vertices, faces);
  ps->setEdgeWidth(1.0);  // quad wireframe: the polygon edges, not a triangulation
  ps->setEdgeColor({0.05f, 0.05f, 0.05f});
  auto* q = ps->addFaceScalarQuantity("corners per face", corners);
  q->setColorMap("viridis");
  q->setMapRange({3.0, 5.0});
  q->setEnabled(true);

  // Criterion 4: dropped files and keyboard shortcuts are reachable. (The real app opens the dropped mesh, A11.)
  std::string last_drop = "(nothing dropped yet)";
  polyscope::state::filesDroppedCallback = [&](const std::vector<std::string>& paths) {
    if (!paths.empty()) last_drop = paths.front();
  };

  polyscope::state::userCallback = [&]() {
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) ps->setEdgeWidth(ps->getEdgeWidth() > 0.0 ? 0.0 : 1.0);  // W: wireframe on/off
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::Begin("RemesherDesk (spike)");
    ImGui::Text("%zu vertices, %zu faces", mesh.vertex_count(), mesh.face_count());
    ImGui::Text("triangles %zu   quads %zu   other %zu", tri, quad, other);
    ImGui::TextDisabled("Polyscope default panels are hidden;");
    ImGui::TextDisabled("this panel is plain ImGui.");
    ImGui::Separator();
    ImGui::Text("W: toggle wireframe");
    ImGui::TextWrapped("Dropped: %s", last_drop.c_str());
    ImGui::End();
  };

  if (!o.screenshot.empty()) {
    polyscope::view::resetCameraToHomeView();
    polyscope::ScreenshotOptions shot;
    shot.transparentBackground = false;
    shot.includeUI = true;  // our own ImGui panel must be in the picture: the default panels are hidden
    polyscope::screenshot(o.screenshot, shot);
    polyscope::shutdown();
    return 0;
  }
  polyscope::show();
  return 0;
}
