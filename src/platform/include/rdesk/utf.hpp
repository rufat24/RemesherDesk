// UTF-8 <-> UTF-16 at the Win32 boundary. UTF-8 is used everywhere else (docs/architecture.md §4).
#pragma once

#include <string>
#include <string_view>

#include "rdesk/result.hpp"

namespace rdesk::platform {

// Strictly validating conversions (invalid input is an error, never silently repaired).
Result<std::wstring> utf8_to_wide(std::string_view s);
Result<std::string> wide_to_utf8(std::wstring_view s);

// True when `s` is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool is_valid_utf8(std::string_view s) noexcept;

}  // namespace rdesk::platform
