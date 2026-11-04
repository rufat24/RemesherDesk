// Minimal smoke test for the scaffold. Returns non-zero on failure.
#include <cstdio>
#include <string_view>

#include "rdesk/version.hpp"

int main() {
  const std::string_view v = rdesk::version_string();
  int failures = 0;
  if (v.empty()) {
    std::fprintf(stderr, "version_string() is empty\n");
    ++failures;
  }
  // Expect MAJOR.MINOR.PATCH (two dots).
  int dots = 0;
  for (char c : v) dots += (c == '.') ? 1 : 0;
  if (dots != 2) {
    std::fprintf(stderr, "version_string() '%.*s' is not MAJOR.MINOR.PATCH\n",
                 static_cast<int>(v.size()), v.data());
    ++failures;
  }
  if (rdesk::kWorkerProtocolVersion != 1 || rdesk::kRdmFormatMajor != 1) {
    std::fprintf(stderr, "unexpected protocol/format version constants\n");
    ++failures;
  }
  if (failures == 0) std::printf("ok: rdesk %.*s\n", static_cast<int>(v.size()), v.data());
  return failures == 0 ? 0 : 1;
}
