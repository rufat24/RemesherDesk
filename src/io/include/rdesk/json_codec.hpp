// JSON encodings of backend parameters and descriptions (the worker protocol and `rdesk backends --json`).
// Lives in rdesk_io so that rdesk_core depends on the standard library only.
#pragma once

#include <string>
#include <string_view>

#include "rdesk/backend.hpp"
#include "rdesk/params.hpp"
#include "rdesk/result.hpp"

namespace rdesk::io {

// `text` must be a JSON object. Booleans, strings and numbers map to bool / string / int64 (when the number is
// written without fraction or exponent and fits) / double. Nulls, arrays and objects are rejected.
// Whether a key exists and whether a type fits is checked later, by complete_params().
// Errc::ParamInvalid on malformed JSON or unsupported value kinds.
Result<ParamMap> params_from_json(std::string_view text);

// Compact JSON object, keys in sorted order.
std::string params_to_json(const ParamMap& params);

// Everything a GUI needs to build its parameter panel.
std::string backend_info_to_json(const BackendInfo& info);

}  // namespace rdesk::io
