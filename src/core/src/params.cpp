#include "rdesk/params.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace rdesk {

const char* to_string(ParamType t) noexcept {
  switch (t) {
    case ParamType::Bool: return "bool";
    case ParamType::Int: return "int";
    case ParamType::Float: return "float";
    case ParamType::Enum: return "enum";
    case ParamType::String: return "string";
  }
  return "string";
}

std::string describe(const ParamValue& v) {
  if (const auto* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
  if (const auto* i = std::get_if<std::int64_t>(&v)) return std::to_string(*i);
  if (const auto* d = std::get_if<double>(&v)) {
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, *d);
    return std::string(buf, r.ptr);
  }
  return "\"" + std::get<std::string>(v) + "\"";
}

namespace {

std::string number(double d) { return describe(ParamValue{d}); }

Error bad(const std::string& key, const std::string& what) {
  std::string detail = "{\"key\":\"";
  for (const char c : key) {
    if (c == '"' || c == '\\') detail += '\\';
    detail += c;
  }
  detail += "\"}";
  return make_error(Errc::ParamInvalid, "parameter '" + key + "': " + what, std::move(detail));
}

std::string type_name(const ParamValue& v) {
  switch (v.index()) {
    case 0: return "a boolean";
    case 1: return "an integer";
    case 2: return "a floating-point number";
    default: return "a string";
  }
}

Result<ParamValue> check_one(const ParamSpec& s, const ParamValue& v) {
  auto expect = [&](const char* what) { return bad(s.key, std::string("expected ") + what + ", got " + type_name(v)); };
  switch (s.type) {
    case ParamType::Bool:
      if (!std::holds_alternative<bool>(v)) return expect("a boolean");
      return v;
    case ParamType::String:
      if (!std::holds_alternative<std::string>(v)) return expect("a string");
      return v;
    case ParamType::Enum: {
      if (!std::holds_alternative<std::string>(v)) return expect("a string (one of the listed values)");
      const std::string& e = std::get<std::string>(v);
      if (std::find(s.enum_values.begin(), s.enum_values.end(), e) == s.enum_values.end()) {
        std::string list;
        for (const auto& x : s.enum_values) list += (list.empty() ? "" : ", ") + x;
        return bad(s.key, "\"" + e + "\" is not one of: " + list);
      }
      return v;
    }
    case ParamType::Int:
    case ParamType::Float: {
      double d;
      ParamValue out = v;
      if (const auto* i = std::get_if<std::int64_t>(&v)) {
        d = static_cast<double>(*i);
        if (s.type == ParamType::Float) out = d;
      } else if (const auto* f = std::get_if<double>(&v)) {
        if (s.type == ParamType::Int) return expect("an integer");
        d = *f;
      } else {
        return expect(s.type == ParamType::Int ? "an integer" : "a number");
      }
      if (!std::isfinite(d)) return bad(s.key, "the value must be finite");
      if (!s.soft_range) {
        if (s.min && d < *s.min) return bad(s.key, describe(v) + " is below the minimum " + number(*s.min));
        if (s.max && d > *s.max) return bad(s.key, describe(v) + " is above the maximum " + number(*s.max));
      }
      return out;
    }
  }
  return bad(s.key, "unsupported parameter type");
}

}  // namespace

Result<ParamMap> complete_params(const std::vector<ParamSpec>& specs, const ParamMap& given) {
  for (const auto& [key, value] : given) {
    (void)value;
    const bool known = std::any_of(specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.key == key; });
    if (!known) return bad(key, "unknown parameter");
  }
  ParamMap out;
  for (const ParamSpec& s : specs) {
    const auto it = given.find(s.key);
    auto checked = check_one(s, it != given.end() ? it->second : s.default_value);
    if (!checked) {
      // A bad default is a bug in the backend description, not in the caller's input.
      if (it == given.end()) return make_error(Errc::Internal, "default of " + checked.error().message, checked.error().detail_json);
      return checked.error();
    }
    out.emplace(s.key, std::move(checked).value());
  }
  return out;
}

}  // namespace rdesk
