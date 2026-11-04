// Error / Result types. No exceptions cross module boundaries (docs/architecture.md §3).
// std::expected is not usable here: clang 18 + libstdc++ 13 cannot compile <expected>.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace rdesk {

enum class Errc {
  Usage,
  IoRead,
  Parse,
  InputInvalid,
  ParamInvalid,
  BackendUnavailable,
  BackendFailed,
  Cancelled,
  IoWrite,
  OutputInvalid,
  Internal
};

// Stable lowercase identifier, used in the worker protocol and in logs.
const char* to_string(Errc code) noexcept;

struct Error {
  Errc code = Errc::Internal;
  std::string message;
  std::string detail_json;  // optional, a JSON object text; empty = none
};

inline Error make_error(Errc code, std::string message, std::string detail_json = {}) {
  return Error{code, std::move(message), std::move(detail_json)};
}

namespace detail {
[[noreturn]] inline void result_misuse(const char* what) noexcept {
  std::fputs("rdesk::Result misuse: ", stderr);
  std::fputs(what, stderr);
  std::fputc('\n', stderr);
  std::abort();
}
}  // namespace detail

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT: implicit by design
  Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & {
    require_ok();
    return std::get<0>(v_);
  }
  const T& value() const& {
    require_ok();
    return std::get<0>(v_);
  }
  T&& value() && {
    require_ok();
    return std::get<0>(std::move(v_));
  }

  const Error& error() const& {
    if (ok()) detail::result_misuse("error() called on an ok Result");
    return std::get<1>(v_);
  }

 private:
  void require_ok() const {
    if (!ok()) detail::result_misuse("value() called on an error Result");
  }
  std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : err_(std::move(error)) {}  // NOLINT

  bool ok() const noexcept { return !err_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  const Error& error() const& {
    if (ok()) detail::result_misuse("error() called on an ok Result");
    return *err_;
  }

 private:
  std::optional<Error> err_;
};

}  // namespace rdesk
