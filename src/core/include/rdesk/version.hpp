// RemesherDesk core — version information (scaffold stub).
#pragma once

#include <string_view>

namespace rdesk {

/// Semantic version of the RemesherDesk core, e.g. "0.0.1".
std::string_view version_string() noexcept;

/// Version of the worker JSON-lines protocol implemented by this build
/// (see docs/architecture.md, "Worker protocol").
constexpr int kWorkerProtocolVersion = 1;

/// Version of the RDM mesh container format this build reads/writes
/// (see docs/rdm-format.md).
constexpr int kRdmFormatMajor = 1;

}  // namespace rdesk
