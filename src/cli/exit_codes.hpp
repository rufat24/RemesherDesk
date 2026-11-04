// Exit codes and protocol error names (docs/architecture.md §4, "Exit codes"). Shared by the CLI and its tests.
#pragma once

#include "rdesk/result.hpp"

namespace rdesk::cli {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitInputRead = 3;
constexpr int kExitInvalid = 4;
constexpr int kExitBackendFailed = 5;
constexpr int kExitCancelled = 6;
constexpr int kExitBackendUnavailable = 7;
constexpr int kExitWrite = 8;
constexpr int kExitInternal = 70;

inline int exit_code_for(Errc c) noexcept {
  switch (c) {
    case Errc::Usage: return kExitUsage;
    case Errc::IoRead:
    case Errc::Parse: return kExitInputRead;
    case Errc::InputInvalid:
    case Errc::ParamInvalid: return kExitInvalid;
    case Errc::BackendFailed:
    case Errc::OutputInvalid: return kExitBackendFailed;
    case Errc::Cancelled: return kExitCancelled;
    case Errc::BackendUnavailable: return kExitBackendUnavailable;
    case Errc::IoWrite: return kExitWrite;
    case Errc::Internal: return kExitInternal;
  }
  return kExitInternal;
}

// "E_USAGE", "E_IO_READ", ... mirrors Errc.
inline const char* error_name(Errc c) noexcept {
  switch (c) {
    case Errc::Usage: return "E_USAGE";
    case Errc::IoRead: return "E_IO_READ";
    case Errc::Parse: return "E_PARSE";
    case Errc::InputInvalid: return "E_INPUT_INVALID";
    case Errc::ParamInvalid: return "E_PARAM_INVALID";
    case Errc::BackendUnavailable: return "E_BACKEND_UNAVAILABLE";
    case Errc::BackendFailed: return "E_BACKEND_FAILED";
    case Errc::Cancelled: return "E_CANCELLED";
    case Errc::IoWrite: return "E_IO_WRITE";
    case Errc::OutputInvalid: return "E_OUTPUT_INVALID";
    case Errc::Internal: return "E_INTERNAL";
  }
  return "E_INTERNAL";
}

}  // namespace rdesk::cli
