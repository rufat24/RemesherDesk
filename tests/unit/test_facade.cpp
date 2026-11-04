#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>

#include "doctest/doctest.h"
#include "fake_backend.hpp"
#include "fixtures.hpp"
#include "rdesk/backend.hpp"
#include "rdesk/convention.hpp"
#include "rdesk/json_codec.hpp"
#include "rdesk/validate.hpp"

using namespace rdesk;
using namespace fixtures;
using Clock = std::chrono::steady_clock;

namespace {

ParamMap iters(std::int64_t n) { return {{"iterations", n}}; }

// A configurable stand-in for a real backend.
class Spy : public IRemeshBackend {
 public:
  explicit Spy(std::string id = "test.spy") {
    info_.id = std::move(id);
    info_.display_name = "spy";
    info_.version = "0";
  }
  const BackendInfo& info() const override { return info_; }
  Result<RemeshOutput> remesh(const PolyMesh& in, const ParamMap& params, ProgressSink&) override {
    seen = in;
    seen_params = params;
    ++calls;
    if (throw_what) throw std::runtime_error(throw_what);
    if (fail) return make_error(Errc::BackendFailed, "spy failed");
    RemeshOutput out;
    out.mesh = reply ? *reply : in;
    return out;
  }
  BackendInfo info_;
  PolyMesh seen;
  ParamMap seen_params;
  int calls = 0;
  const char* throw_what = nullptr;
  bool fail = false;
  std::optional<PolyMesh> reply;
};

struct Registered {
  std::shared_ptr<Spy> spy;
  explicit Registered(std::shared_ptr<Spy> s) : spy(std::move(s)) { register_backend(spy); }
  ~Registered() { unregister_backend(spy->info().id); }
};

}  // namespace

TEST_CASE("params: defaults are filled and values are checked") {
  backends::register_fake_backend();
  const auto& specs = find_backend("fake.subdivide")->info().params;

  auto d = complete_params(specs, {});
  REQUIRE(d.ok());
  CHECK(std::get<std::int64_t>(d.value().at("iterations")) == 1);
  CHECK(std::get<bool>(d.value().at("debug.fail")) == false);
  CHECK(d.value().size() == specs.size());

  auto set = complete_params(specs, iters(3));
  REQUIRE(set.ok());
  CHECK(std::get<std::int64_t>(set.value().at("iterations")) == 3);
}

TEST_CASE("params: errors name the key") {
  backends::register_fake_backend();
  const auto& specs = find_backend("fake.subdivide")->info().params;
  auto names_key = [&](const ParamMap& m, const char* key) {
    auto r = complete_params(specs, m);
    return !r.ok() && r.error().code == Errc::ParamInvalid && r.error().message.find(key) != std::string::npos &&
           r.error().detail_json.find(key) != std::string::npos;
  };
  CHECK(names_key({{"nope", std::int64_t{1}}}, "nope"));                 // unknown key
  CHECK(names_key({{"iterations", std::int64_t{9}}}, "iterations"));     // above max
  CHECK(names_key({{"iterations", std::int64_t{-1}}}, "iterations"));    // below min
  CHECK(names_key({{"iterations", 2.5}}, "iterations"));                 // float for int
  CHECK(names_key({{"iterations", std::string("two")}}, "iterations"));  // string for int
  CHECK(names_key({{"debug.fail", std::int64_t{1}}}, "debug.fail"));     // int for bool
}

TEST_CASE("params: float, enum, string, soft range and non-finite values") {
  ParamSpec f;
  f.key = "scale";
  f.type = ParamType::Float;
  f.default_value = 1.0;
  f.min = 0.0;
  f.max = 2.0;
  ParamSpec e;
  e.key = "mode";
  e.type = ParamType::Enum;
  e.default_value = std::string("fast");
  e.enum_values = {"fast", "good"};
  ParamSpec s;
  s.key = "label";
  s.type = ParamType::String;
  s.default_value = std::string();
  ParamSpec soft;
  soft.key = "count";
  soft.type = ParamType::Int;
  soft.default_value = std::int64_t{10};
  soft.min = 1;
  soft.max = 100;
  soft.soft_range = true;
  const std::vector<ParamSpec> specs = {f, e, s, soft};

  auto ok = complete_params(specs, {{"scale", std::int64_t{2}}, {"mode", std::string("good")}, {"count", std::int64_t{5000}}});
  REQUIRE(ok.ok());
  CHECK(std::get<double>(ok.value().at("scale")) == 2.0);  // an integer is accepted for a float parameter and widened
  CHECK(std::get<std::int64_t>(ok.value().at("count")) == 5000);  // soft range: accepted

  CHECK_FALSE(complete_params(specs, {{"scale", 2.5}}).ok());
  CHECK_FALSE(complete_params(specs, {{"scale", std::numeric_limits<double>::quiet_NaN()}}).ok());
  CHECK_FALSE(complete_params(specs, {{"scale", std::numeric_limits<double>::infinity()}}).ok());
  auto bad_enum = complete_params(specs, {{"mode", std::string("slow")}});
  REQUIRE_FALSE(bad_enum.ok());
  CHECK(bad_enum.error().message.find("fast, good") != std::string::npos);
  CHECK_FALSE(complete_params(specs, {{"mode", std::int64_t{0}}}).ok());
  CHECK_FALSE(complete_params(specs, {{"label", true}}).ok());
}

TEST_CASE("params: a broken default in a backend description is an internal error, not the caller's fault") {
  ParamSpec s;
  s.key = "n";
  s.type = ParamType::Int;
  s.default_value = std::string("oops");
  auto r = complete_params({s}, {});
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == Errc::Internal);
}

TEST_CASE("json codec: params round trip and errors") {
  auto p = io::params_from_json(R"({"iterations":2,"scale":0.5,"mode":"good","debug.fail":true})");
  REQUIRE(p.ok());
  CHECK(std::get<std::int64_t>(p.value().at("iterations")) == 2);
  CHECK(std::get<double>(p.value().at("scale")) == 0.5);
  CHECK(std::get<std::string>(p.value().at("mode")) == "good");
  CHECK(std::get<bool>(p.value().at("debug.fail")) == true);
  auto back = io::params_from_json(io::params_to_json(p.value()));
  REQUIRE(back.ok());
  CHECK(back.value() == p.value());

  CHECK(std::holds_alternative<double>(io::params_from_json(R"({"x":2.0})").value().at("x")));  // 2.0 is a float
  for (const char* bad : {"", "[1]", "{", R"({"a":null})", R"({"a":[1]})", R"({"a":{"b":1}})", R"({"a":18446744073709551615})"}) {
    auto r = io::params_from_json(bad);
    CHECK_MESSAGE(!r.ok(), bad);
    if (!r.ok()) CHECK(r.error().code == Errc::ParamInvalid);
  }
}

TEST_CASE("json codec: backend description carries what a GUI needs") {
  backends::register_fake_backend();
  const std::string j = io::backend_info_to_json(find_backend("fake.subdivide")->info());
  CHECK(j.find("\"id\":\"fake.subdivide\"") != std::string::npos);
  CHECK(j.find("\"key\":\"iterations\"") != std::string::npos);
  CHECK(j.find("\"min\":0") != std::string::npos);
  CHECK(j.find("\"max\":4") != std::string::npos);
  CHECK(j.find("\"advanced\":true") != std::string::npos);
  CHECK(j.find("\"cooperative_cancel\":true") != std::string::npos);
}

TEST_CASE("registry: register, replace, find, unregister") {
  auto a = std::make_shared<Spy>("test.registry");
  register_backend(a);
  CHECK(find_backend("test.registry") == a);
  auto b = std::make_shared<Spy>("test.registry");
  register_backend(b);
  CHECK(find_backend("test.registry") == b);  // replaced, not duplicated
  std::size_t n = 0;
  for (const auto& x : registered_backends()) n += x->info().id == "test.registry";
  CHECK(n == 1);
  CHECK(unregister_backend("test.registry"));
  CHECK_FALSE(unregister_backend("test.registry"));
  CHECK(find_backend("test.registry") == nullptr);
}

TEST_CASE("facade: one iteration turns a cube into 24 quads") {
  backends::register_fake_backend();
  NullProgressSink sink;
  auto r = remesh("fake.subdivide", make_cube(), iters(1), sink);
  REQUIRE(r.ok());
  const PolyMesh& m = r.value().mesh;
  CHECK(m.face_count() == 24);
  for (std::size_t f = 0; f < m.face_count(); ++f) CHECK(m.face_size(f) == 4);
  CHECK(m.vertex_count() == 26);  // 8 corners + 12 edge midpoints + 6 face points
  const ValidationReport v = validate(m);
  CHECK(v.is_closed_manifold());
  CHECK(v.topology.euler == 2);
  CHECK(signed_volume(m) == doctest::Approx(1.0));  // the new vertices lie on the old faces: same surface, same outward winding
  CHECK(m.convention == Convention::RightHandedYUpCCW);
}

TEST_CASE("facade: iterations 0, 2 and 4; groups are inherited; output is deterministic") {
  backends::register_fake_backend();
  NullProgressSink sink;
  PolyMesh in = make_cube();
  in.face_group = {0, 0, 1, 1, 2, 2};
  CHECK(remesh("fake.subdivide", in, iters(0), sink).value().mesh.face_count() == 6);
  auto two = remesh("fake.subdivide", in, iters(2), sink);
  REQUIRE(two.ok());
  CHECK(two.value().mesh.face_count() == 96);
  REQUIRE(two.value().mesh.face_group.size() == 96);
  CHECK(two.value().mesh.face_group.front() == 0);
  CHECK(two.value().mesh.face_group.back() == 2);
  CHECK(remesh("fake.subdivide", in, iters(4), sink).value().mesh.face_count() == 6 * 256);
  auto again = remesh("fake.subdivide", in, iters(2), sink);
  CHECK(bit_equal(again.value().mesh.positions, two.value().mesh.positions));
  CHECK(again.value().mesh.corners == two.value().mesh.corners);
}

TEST_CASE("facade: polygons are split into one quad per corner; a Unity-convention input comes back as Unity") {
  backends::register_fake_backend();
  NullProgressSink sink;
  PolyMesh pent = make_mesh({0, 0, 0, 2, 0, 0, 3, 1, 0, 1.5f, 2, 0, -1, 1, 0}, {{0, 1, 2, 3, 4}});
  auto r = remesh("fake.subdivide", pent, iters(1), sink);
  REQUIRE(r.ok());
  CHECK(r.value().mesh.face_count() == 5);

  const PolyMesh unity = to_convention(make_cube(), Convention::UnityLeftHandedYUpCW);
  auto u = remesh("fake.subdivide", unity, iters(1), sink);
  REQUIRE(u.ok());
  CHECK(u.value().mesh.convention == Convention::UnityLeftHandedYUpCW);
  auto rh = remesh("fake.subdivide", make_cube(), iters(1), sink);
  REQUIRE(rh.ok());
  // Same geometry either way: converting the RH result gives exactly the Unity result.
  const PolyMesh expected = to_convention(rh.value().mesh, Convention::UnityLeftHandedYUpCW);
  CHECK(bit_equal(u.value().mesh.positions, expected.positions));
  CHECK(u.value().mesh.corners == expected.corners);
}

TEST_CASE("facade: error mapping") {
  backends::register_fake_backend();
  NullProgressSink sink;
  SUBCASE("debug.fail -> BackendFailed") {
    auto r = remesh("fake.subdivide", make_cube(), {{"debug.fail", true}}, sink);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == Errc::BackendFailed);
  }
  SUBCASE("unknown backend -> BackendUnavailable") {
    CHECK(remesh("no.such", make_cube(), {}, sink).error().code == Errc::BackendUnavailable);
  }
  SUBCASE("bad parameter -> ParamInvalid naming the key") {
    auto r = remesh("fake.subdivide", make_cube(), iters(99), sink);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == Errc::ParamInvalid);
    CHECK(r.error().message.find("iterations") != std::string::npos);
  }
  SUBCASE("invalid input -> InputInvalid") {
    PolyMesh bad = make_cube();
    bad.corners[0] = 99;
    CHECK(remesh("fake.subdivide", bad, {}, sink).error().code == Errc::InputInvalid);
    PolyMesh nan = make_cube();
    nan.positions[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(remesh("fake.subdivide", nan, {}, sink).error().code == Errc::InputInvalid);
    PolyMesh empty;
    empty.face_offsets = {0};
    CHECK(remesh("fake.subdivide", empty, {}, sink).error().code == Errc::InputInvalid);
  }
}

TEST_CASE("facade: the backend sees its own convention and the result is converted back") {
  auto spy = std::make_shared<Spy>("test.spy.unity");
  spy->info_.expected_convention = Convention::UnityLeftHandedYUpCW;
  Registered reg(spy);
  NullProgressSink sink;
  const PolyMesh rh = make_chiral();
  auto r = remesh("test.spy.unity", rh, {}, sink);
  REQUIRE(r.ok());
  REQUIRE(spy->calls == 1);
  CHECK(spy->seen.convention == Convention::UnityLeftHandedYUpCW);
  CHECK(bit_equal(spy->seen.positions, to_convention(rh, Convention::UnityLeftHandedYUpCW).positions));
  CHECK(spy->seen.corners == to_convention(rh, Convention::UnityLeftHandedYUpCW).corners);
  // The spy returned its input unchanged; the facade converts it back to the caller's convention.
  CHECK(r.value().mesh.convention == Convention::RightHandedYUpCCW);
  CHECK(bit_equal(r.value().mesh.positions, rh.positions));
  CHECK(r.value().mesh.corners == rh.corners);
}

TEST_CASE("facade: no conversion when the conventions already agree") {
  auto spy = std::make_shared<Spy>("test.spy.rh");
  Registered reg(spy);
  NullProgressSink sink;
  const PolyMesh rh = make_chiral();
  REQUIRE(remesh("test.spy.rh", rh, {}, sink).ok());
  CHECK(bit_equal(spy->seen.positions, rh.positions));
}

TEST_CASE("facade: the backend's input requirements are enforced before it runs") {
  auto spy = std::make_shared<Spy>("test.spy.req");
  Registered reg(spy);
  NullProgressSink sink;
  const PolyMesh open_quad = make_grid(1, 1);
  const PolyMesh shared_edge = weld_by_position(append(make_cube(), make_cube(1, 1, 0))).value();

  SUBCASE("triangles_only") {
    spy->info_.requirements.triangles_only = true;
    CHECK(remesh("test.spy.req", make_cube(), {}, sink).error().code == Errc::InputInvalid);
    CHECK(remesh("test.spy.req", triangulate(make_cube()), {}, sink).ok());
  }
  SUBCASE("require_closed") {
    spy->info_.requirements.require_closed = true;
    auto r = remesh("test.spy.req", open_quad, {}, sink);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == Errc::InputInvalid);
    CHECK(r.error().message.find("boundary") != std::string::npos);
    CHECK(remesh("test.spy.req", make_cube(), {}, sink).ok());
  }
  SUBCASE("require_manifold") {
    spy->info_.requirements.require_manifold = true;
    auto r = remesh("test.spy.req", shared_edge, {}, sink);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == Errc::InputInvalid);
    CHECK(r.error().message.find("non-manifold") != std::string::npos);
    CHECK(remesh("test.spy.req", make_cube(), {}, sink).ok());
  }
  CHECK(spy->calls <= 2);  // the failing calls never reached the backend
}

TEST_CASE("facade: a throwing or invalid backend cannot corrupt the caller") {
  auto spy = std::make_shared<Spy>("test.spy.bad");
  Registered reg(spy);
  NullProgressSink sink;
  SUBCASE("exception") {
    spy->throw_what = "boom";
    auto r = remesh("test.spy.bad", make_cube(), {}, sink);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == Errc::BackendFailed);
    CHECK(r.error().message.find("boom") != std::string::npos);
  }
  SUBCASE("error result passes through") {
    spy->fail = true;
    CHECK(remesh("test.spy.bad", make_cube(), {}, sink).error().code == Errc::BackendFailed);
  }
  SUBCASE("output with an out-of-range index") {
    PolyMesh bad = make_cube();
    bad.corners[3] = 1000;
    spy->reply = bad;
    CHECK(remesh("test.spy.bad", make_cube(), {}, sink).error().code == Errc::OutputInvalid);
  }
  SUBCASE("output with a non-finite coordinate") {
    PolyMesh bad = make_cube();
    bad.positions[5] = std::numeric_limits<float>::infinity();
    spy->reply = bad;
    CHECK(remesh("test.spy.bad", make_cube(), {}, sink).error().code == Errc::OutputInvalid);
  }
}

TEST_CASE("facade: cancel before the call never reaches the backend") {
  auto spy = std::make_shared<Spy>("test.spy.cancel");
  Registered reg(spy);
  NullProgressSink sink;
  sink.request_cancel();
  auto r = remesh("test.spy.cancel", make_cube(), {}, sink);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == Errc::Cancelled);
  CHECK(spy->calls == 0);
}

TEST_CASE("facade: a cancel set from another thread is honoured within 100 ms") {
  backends::register_fake_backend();
  struct Case {
    const char* name;
    PolyMesh mesh;
    ParamMap params;
  };
  // (a) a backend that is asleep: the nap wakes on cancel.  (b) a backend that is computing a large subdivision.
  std::vector<Case> cases;
  cases.push_back({"sleeping", make_cube(), {{"iterations", std::int64_t{4}}, {"debug.sleep_ms_per_iter", std::int64_t{60000}}}});
  cases.push_back({"computing", make_grid(96, 96), {{"iterations", std::int64_t{4}}}});

  for (const Case& c : cases) {
    INFO("case: " << c.name);
    NullProgressSink sink;
    std::atomic<Clock::time_point> cancelled_at{Clock::time_point{}};
    std::thread canceller([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      cancelled_at.store(Clock::now());
      sink.request_cancel();
    });
    auto r = remesh("fake.subdivide", c.mesh, c.params, sink);
    const auto returned_at = Clock::now();
    canceller.join();
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == Errc::Cancelled);
    const double ms = std::chrono::duration<double, std::milli>(returned_at - cancelled_at.load()).count();
    CHECK_MESSAGE(ms <= 100.0, "returned " << ms << " ms after the cancel");
  }
}

TEST_CASE("facade: a backend that ignores cancel finishes, and the facade still reports Cancelled") {
  backends::register_fake_backend();
  // A cancel that is already set stops the facade before the backend starts.
  NullProgressSink early;
  early.request_cancel();
  CHECK(remesh("fake.subdivide", make_cube(), {{"debug.ignore_cancel", true}}, early).error().code == Errc::Cancelled);

  // A cancel that arrives while the backend runs is reported even though a deaf backend completed.
  class CancelOnProgress : public NullProgressSink {
   public:
    void progress(std::string_view, std::optional<double>) override { request_cancel(); }
  } mid;
  auto r = remesh("fake.subdivide", make_cube(), {{"debug.ignore_cancel", true}}, mid);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == Errc::Cancelled);
}
