// Parameter files: parsing never crashes; whatever parses survives a write/read round trip unchanged; checking a
// parsed map against a real backend description either completes it or names a key.
#include <cstdint>
#include <string_view>

#include "check.hpp"
#include "fake_backend.hpp"
#include "rdesk/json_codec.hpp"

using namespace rdesk;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static const auto backend = backends::make_fake_backend();
  auto parsed = io::params_from_json(std::string_view(reinterpret_cast<const char*>(data), size));
  if (!parsed) {
    FUZZ_CHECK(parsed.error().code == Errc::ParamInvalid);
    return 0;
  }
  auto full = complete_params(backend->info().params, parsed.value());
  if (full) {
    FUZZ_CHECK(full.value().size() == backend->info().params.size());
  } else {
    FUZZ_CHECK(full.error().code == Errc::ParamInvalid);
    FUZZ_CHECK(!full.error().message.empty());
  }
  auto again = io::params_from_json(io::params_to_json(parsed.value()));
  FUZZ_CHECK(again.ok());
  FUZZ_CHECK(again.value() == parsed.value());
  return 0;
}
