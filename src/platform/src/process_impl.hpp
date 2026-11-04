// Internal: the per-OS half of rdesk::platform::Process. process.cpp holds everything that is the same on all
// platforms (reader threads, line queue, waiting); process_posix.cpp / process_win32.cpp implement these functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "rdesk/process.hpp"

namespace rdesk::platform::detail {

struct Native;  // defined by the OS file
struct NativeDeleter {
  void operator()(Native*) const noexcept;  // closes every handle; never kills (the owner does that first)
};
using NativePtr = std::unique_ptr<Native, NativeDeleter>;

Result<NativePtr> native_spawn(const ProcessOptions& options);
std::int64_t native_pid(const Native&) noexcept;
// Blocking read from the child's stdout (stream 1) or stderr (stream 2): bytes read, 0 at end of stream, < 0 on error.
long native_read(Native&, int stream, char* buf, std::size_t n) noexcept;
bool native_write_stdin(Native&, const char* data, std::size_t n) noexcept;
void native_close_stdin(Native&) noexcept;
void native_kill(Native&) noexcept;                   // the whole tree, idempotent, does not wait
bool native_poll_exit(Native&, ExitStatus&) noexcept; // non-blocking; true once the child has ended
void native_sleep_until_exit(Native&, int max_ms) noexcept;  // returns early when the child ends; may return after < max_ms
void native_abandon(Native&) noexcept;

}  // namespace rdesk::platform::detail
