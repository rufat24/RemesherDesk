#include "rdesk/version.hpp"

#ifndef RDESK_VERSION_STRING
#error "RDESK_VERSION_STRING must be defined by the build system"
#endif

namespace rdesk {

std::string_view version_string() noexcept { return RDESK_VERSION_STRING; }

}  // namespace rdesk
