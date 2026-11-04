#include "rdesk/utf.hpp"

#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rdesk::platform {

bool is_valid_utf8(std::string_view s) noexcept {
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len;
    std::uint32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (i + len > n) return false;
    for (std::size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;  // overlong
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

#ifdef _WIN32

Result<std::wstring> utf8_to_wide(std::string_view s) {
  if (s.empty()) return std::wstring();
  if (s.size() > static_cast<std::size_t>(INT32_MAX)) return make_error(Errc::Internal, "string too long");
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return make_error(Errc::InputInvalid, "not valid UTF-8");
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

Result<std::string> wide_to_utf8(std::wstring_view s) {
  if (s.empty()) return std::string();
  if (s.size() > static_cast<std::size_t>(INT32_MAX)) return make_error(Errc::Internal, "string too long");
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return make_error(Errc::InputInvalid, "not valid UTF-16");
  std::string u(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), u.data(), n, nullptr, nullptr);
  return u;
}

#else  // wchar_t is 32-bit UTF-32 here; the conversion is only used for tests and tools.

Result<std::wstring> utf8_to_wide(std::string_view s) {
  if (!is_valid_utf8(s)) return make_error(Errc::InputInvalid, "not valid UTF-8");
  std::wstring w;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp;
    std::size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
    else { cp = c & 0x07u; len = 4; }
    for (std::size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    w.push_back(static_cast<wchar_t>(cp));
    i += len;
  }
  return w;
}

Result<std::string> wide_to_utf8(std::wstring_view s) {
  std::string u;
  for (const wchar_t wc : s) {
    const auto cp = static_cast<std::uint32_t>(wc);
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return make_error(Errc::InputInvalid, "not valid Unicode");
    if (cp < 0x80) {
      u.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      u.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      u.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      u.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      u.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      u.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      u.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return u;
}

#endif

}  // namespace rdesk::platform
