// Child process control: spawn with piped stdin / stdout / stderr, line-oriented reading with timeouts, forced
// kill of the whole process tree, exit status including crashes (docs/architecture.md §4, "Platform details").
//
// Windows: CreateProcessW, only the three pipe handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), the child is
// assigned to a Job Object with KILL_ON_JOB_CLOSE before it runs, so it dies with the parent and kill() takes down
// grandchildren too. POSIX: posix_spawn in its own process group (kill() signals the group), pipes are close-on-exec
// and on glibc >= 2.34 every other descriptor is closed in the child. SIGPIPE is set to "ignore" in the parent
// the first time a process is spawned (the child gets the default disposition back).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rdesk/result.hpp"

namespace rdesk::platform {

struct ProcessOptions {
  std::filesystem::path exe;       // path of the executable (not searched on PATH)
  std::vector<std::string> args;   // UTF-8, without the program name
  std::filesystem::path cwd;       // empty = inherit
};

struct ExitStatus {
  bool exited_normally = false;  // the process called exit / returned from main
  bool crashed = false;          // died from a signal (POSIX) or an NTSTATUS error code such as 0xC0000005 (Windows)
  int code = 0;                  // exit code when exited_normally
  int signal = 0;                // POSIX signal number when crashed or killed
  std::uint32_t raw = 0;         // Windows: the full exit code; POSIX: the wait status
  bool killed_by_us = false;     // Process::kill() was called before the process ended
  std::string describe() const; // for example "exit 6", "crashed (signal 11)", "crashed (0xC0000005)", "killed"
};

enum class LineStatus { Line, Timeout, Eof };

class Process {
 public:
  // Errc::BackendUnavailable when the program cannot be started (missing, not executable, wrong architecture).
  static Result<std::unique_ptr<Process>> spawn(const ProcessOptions& options);
  ~Process();  // kills the process tree if it is still running, then releases everything
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  std::int64_t pid() const noexcept;

  // Next line from the child's stdout, without the line terminator ("\n" or "\r\n"). A final unterminated line is
  // delivered at EOF. Lines are queued by a reader thread (at most 100,000 queued; beyond that the oldest
  // is dropped and counted in dropped_stdout_lines()). timeout_ms < 0 waits forever.
  LineStatus read_stdout_line(std::string& line, int timeout_ms);
  std::size_t dropped_stdout_lines() const;

  // Everything the child wrote to stderr so far (at most 4 MiB; older text is cut and a marker is prepended).
  std::string stderr_text() const;

  // False when the child closed its stdin or has exited.
  bool write_stdin(std::string_view data);
  void close_stdin();

  // Forced end of the child and everything it started. Idempotent. Does not wait.
  void kill();

  std::optional<ExitStatus> try_wait();
  // nullopt on timeout. timeout_ms < 0 waits forever.
  std::optional<ExitStatus> wait(int timeout_ms);

  // Simulates the parent dying: releases the OS object that ties the child's lifetime to ours. Windows: closes the
  // Job Object handle, whose KILL_ON_JOB_CLOSE ends the child. POSIX: there is no equivalent, this does nothing.
  void abandon();

  struct Impl;

 private:
  Process();
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdesk::platform
