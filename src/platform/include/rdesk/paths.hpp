// Well-known locations (docs/architecture.md §4: "Job folder").
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "rdesk/result.hpp"

namespace rdesk::platform {

// UTF-8 text <-> filesystem path. On Windows the conversion goes through UTF-16 (std::filesystem::path::string()
// would use the ANSI code page and mangle non-ASCII names; u8path is deprecated). Invalid UTF-8 gives an empty path
// from path_from_utf8 (callers that care use is_valid_utf8 first).
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& p);

// Absolute path of the running executable.
Result<std::filesystem::path> executable_path();

// Windows: %LOCALAPPDATA%\RemesherDesk\jobs.  Linux: $XDG_STATE_HOME/remesherdesk/jobs, else ~/.local/state/remesherdesk/jobs.
// Not created.
Result<std::filesystem::path> default_jobs_root();

// Creates <root>/<yyyymmdd-hhmmss>-<4 hex> (the folder is unique even for two calls in one second) and returns it.
Result<std::filesystem::path> make_job_dir(const std::filesystem::path& root);

// Deletes the oldest job folders so that at most `keep` remain. Only directories whose names match the job pattern
// are considered; nothing else under `root` is touched. Returns how many were removed.
std::size_t prune_job_dirs(const std::filesystem::path& root, std::size_t keep);

}  // namespace rdesk::platform
