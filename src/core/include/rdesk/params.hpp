// Backend parameters: specification, defaults and validation (docs/architecture.md §3).
// JSON (de)serialisation lives in rdesk_io (json_codec.hpp) so that core depends on the standard library only.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "rdesk/result.hpp"

namespace rdesk {

enum class ParamType { Bool, Int, Float, Enum, String };
const char* to_string(ParamType t) noexcept;

using ParamValue = std::variant<bool, std::int64_t, double, std::string>;
using ParamMap = std::map<std::string, ParamValue>;

struct ParamSpec {
  std::string key;  // stable id, for example "target_face_count"
  std::string label, group, tooltip;
  ParamType type = ParamType::Int;
  ParamValue default_value = std::int64_t{0};
  std::optional<double> min, max, step;  // numeric types only
  bool soft_range = false;               // true: min/max are UI hints and values outside them are accepted
  std::vector<std::string> enum_values;  // Enum only
  bool advanced = false;                 // collapsed by default in the GUI
};

// Readable form of a value for messages ("true", "3", "0.5", "\"abc\"").
std::string describe(const ParamValue& v);

// Returns `given` completed with the defaults of every parameter not mentioned.
// Errc::ParamInvalid, with the offending key in the message and in detail_json ({"key":"..."}), for:
// an unknown key; a value of the wrong type (an integer is accepted for a Float parameter, nothing else is
// converted); an Enum value that is not listed; a non-finite number; a value outside [min, max] unless the
// parameter has soft_range.
Result<ParamMap> complete_params(const std::vector<ParamSpec>& specs, const ParamMap& given);

}  // namespace rdesk
