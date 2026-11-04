#include "rdesk/json_codec.hpp"

#include <cstdint>
#include <exception>

#include <nlohmann/json.hpp>

namespace rdesk::io {

namespace {
using json = nlohmann::json;

json value_to_json(const ParamValue& v) {
  if (const auto* b = std::get_if<bool>(&v)) return *b;
  if (const auto* i = std::get_if<std::int64_t>(&v)) return *i;
  if (const auto* d = std::get_if<double>(&v)) return *d;
  return std::get<std::string>(v);
}
}  // namespace

Result<ParamMap> params_from_json(std::string_view text) {
  try {
    json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return make_error(Errc::ParamInvalid, "parameters are not valid JSON");
    if (!j.is_object()) return make_error(Errc::ParamInvalid, "parameters must be a JSON object");
    ParamMap out;
    for (auto it = j.begin(); it != j.end(); ++it) {
      const json& v = it.value();
      const std::string& key = it.key();
      auto bad = [&](const char* what) {
        return make_error(Errc::ParamInvalid, "parameter '" + key + "': " + what, "{\"key\":" + json(key).dump(-1, ' ', false, json::error_handler_t::replace) + "}");
      };
      if (v.is_boolean()) out[key] = v.get<bool>();
      else if (v.is_number_integer()) {
        if (v.is_number_unsigned() && v.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) return bad("integer is too large");
        out[key] = v.get<std::int64_t>();
      } else if (v.is_number_float()) out[key] = v.get<double>();
      else if (v.is_string()) out[key] = v.get<std::string>();
      else return bad("must be a boolean, number or string");
    }
    return out;
  } catch (const std::exception& e) {
    return make_error(Errc::ParamInvalid, std::string("cannot read parameters: ") + e.what());
  }
}

std::string params_to_json(const ParamMap& params) {
  json j = json::object();
  for (const auto& [k, v] : params) j[k] = value_to_json(v);
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string backend_info_to_json(const BackendInfo& info) {
  json j;
  j["id"] = info.id;
  j["display_name"] = info.display_name;
  j["version"] = info.version;
  j["convention"] = to_string(info.expected_convention);
  j["caps"] = {{"progress", info.caps.progress},
               {"cooperative_cancel", info.caps.cooperative_cancel},
               {"deterministic", info.caps.deterministic},
               {"thread_safe", info.caps.thread_safe},
               {"max_threads", info.caps.max_threads}};
  j["requirements"] = {{"require_manifold", info.requirements.require_manifold},
                       {"require_closed", info.requirements.require_closed},
                       {"triangles_only", info.requirements.triangles_only}};
  j["params"] = json::array();
  for (const ParamSpec& s : info.params) {
    json p;
    p["key"] = s.key;
    p["label"] = s.label;
    p["group"] = s.group;
    p["tooltip"] = s.tooltip;
    p["type"] = to_string(s.type);
    p["default"] = value_to_json(s.default_value);
    if (s.min) p["min"] = *s.min;
    if (s.max) p["max"] = *s.max;
    if (s.step) p["step"] = *s.step;
    p["soft_range"] = s.soft_range;
    if (s.type == ParamType::Enum) p["enum_values"] = s.enum_values;
    p["advanced"] = s.advanced;
    j["params"].push_back(std::move(p));
  }
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace rdesk::io
