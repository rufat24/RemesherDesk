#include <algorithm>
#include <cstdio>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "process_impl.hpp"
#include "rdesk/process.hpp"

namespace rdesk::platform {

namespace {
constexpr std::size_t kMaxQueuedLines = 100000;
constexpr std::size_t kMaxStderrBytes = 4u * 1024u * 1024u;
using Clock = std::chrono::steady_clock;
}  // namespace

std::string ExitStatus::describe() const {
  if (killed_by_us) return "killed";
  if (exited_normally) return "exit " + std::to_string(code);
  if (crashed) {
    if (signal != 0) return "crashed (signal " + std::to_string(signal) + ")";
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%08X", static_cast<unsigned>(raw));
    return std::string("crashed (") + buf + ")";
  }
  return "ended (" + std::to_string(raw) + ")";
}

struct Process::Impl {
  detail::NativePtr native;
  std::thread out_thread, err_thread;

  mutable std::mutex m;
  std::condition_variable cv;        // lines available / end of stream
  std::condition_variable space_cv;  // room in the queue / shutting down
  std::deque<std::string> lines;
  bool out_eof = false;
  bool stopping = false;
  std::string err;
  bool err_cut = false;

  std::mutex state_m;  // guards exit / killed
  std::optional<ExitStatus> exit;
  bool killed = false;

  void pump_stdout() {
    std::string partial;
    char buf[65536];
    for (;;) {
      const long n = detail::native_read(*native, 1, buf, sizeof buf);
      if (n <= 0) break;
      partial.append(buf, static_cast<std::size_t>(n));
      std::size_t start = 0, nl;
      std::vector<std::string> ready;
      while ((nl = partial.find('\n', start)) != std::string::npos) {
        std::string line = partial.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ready.push_back(std::move(line));
        start = nl + 1;
      }
      partial.erase(0, start);
      if (!ready.empty()) {
        std::unique_lock<std::mutex> lock(m);
        for (auto& l : ready) {
          // Backpressure instead of loss: while the consumer is behind, stop reading; the pipe fills up and the
          // child blocks in its own write. Memory stays bounded and no line is ever dropped.
          space_cv.wait(lock, [&] { return lines.size() < kMaxQueuedLines || stopping; });
          if (stopping) break;
          lines.push_back(std::move(l));
          cv.notify_all();
        }
        if (stopping) break;
      }
    }
    std::lock_guard<std::mutex> lock(m);
    if (!partial.empty()) {
      if (partial.back() == '\r') partial.pop_back();
      lines.push_back(std::move(partial));
    }
    out_eof = true;
    cv.notify_all();
  }

  void pump_stderr() {
    char buf[16384];
    for (;;) {
      const long n = detail::native_read(*native, 2, buf, sizeof buf);
      if (n <= 0) break;
      std::lock_guard<std::mutex> lock(m);
      err.append(buf, static_cast<std::size_t>(n));
      if (err.size() > kMaxStderrBytes) {
        err.erase(0, err.size() - kMaxStderrBytes);
        err_cut = true;
      }
    }
  }

  // Updates `exit` if the child has ended.
  std::optional<ExitStatus> poll() {
    std::lock_guard<std::mutex> lock(state_m);
    if (exit) return exit;
    ExitStatus st;
    if (detail::native_poll_exit(*native, st)) {
      st.killed_by_us = killed;
      exit = st;
    }
    return exit;
  }
};

Process::Process() = default;

Result<std::unique_ptr<Process>> Process::spawn(const ProcessOptions& options) {
  auto native = detail::native_spawn(options);
  if (!native) return native.error();
  std::unique_ptr<Process> p(new Process());
  p->impl_ = std::make_unique<Impl>();
  p->impl_->native = std::move(native).value();
  Impl* impl = p->impl_.get();
  impl->out_thread = std::thread([impl] { impl->pump_stdout(); });
  impl->err_thread = std::thread([impl] { impl->pump_stderr(); });
  return p;
}

Process::~Process() {
  if (!impl_) return;
  detail::native_kill(*impl_->native);
  {
    std::lock_guard<std::mutex> lock(impl_->state_m);
    impl_->killed = !impl_->exit.has_value() || impl_->killed;
  }
  // Give the OS a moment to tear the tree down so that the reader threads see end-of-stream.
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (!impl_->poll() && Clock::now() < deadline) detail::native_sleep_until_exit(*impl_->native, 5);
  detail::native_close_stdin(*impl_->native);
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->stopping = true;  // wakes a reader that waits for queue space
  }
  impl_->space_cv.notify_all();
  if (impl_->out_thread.joinable()) impl_->out_thread.join();
  if (impl_->err_thread.joinable()) impl_->err_thread.join();
}

std::int64_t Process::pid() const noexcept { return detail::native_pid(*impl_->native); }

LineStatus Process::read_stdout_line(std::string& line, int timeout_ms) {
  std::unique_lock<std::mutex> lock(impl_->m);
  auto ready = [&] { return !impl_->lines.empty() || impl_->out_eof; };
  if (timeout_ms < 0) {
    impl_->cv.wait(lock, ready);
  } else if (!impl_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
    return LineStatus::Timeout;
  }
  if (!impl_->lines.empty()) {
    line = std::move(impl_->lines.front());
    impl_->lines.pop_front();
    impl_->space_cv.notify_one();
    return LineStatus::Line;
  }
  return LineStatus::Eof;
}

std::string Process::stderr_text() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->err_cut ? "[earlier stderr output was cut]\n" + impl_->err : impl_->err;
}

bool Process::write_stdin(std::string_view data) {
  return detail::native_write_stdin(*impl_->native, data.data(), data.size());
}

void Process::close_stdin() { detail::native_close_stdin(*impl_->native); }

void Process::kill() {
  {
    std::lock_guard<std::mutex> lock(impl_->state_m);
    if (!impl_->exit) impl_->killed = true;
  }
  detail::native_kill(*impl_->native);
}

std::optional<ExitStatus> Process::try_wait() { return impl_->poll(); }

std::optional<ExitStatus> Process::wait(int timeout_ms) {
  const auto deadline = timeout_ms < 0 ? Clock::time_point::max() : Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (auto st = impl_->poll()) return st;
    const auto now = Clock::now();
    if (now >= deadline) return std::nullopt;
    const auto left = timeout_ms < 0 ? 5 : static_cast<int>(std::min<long long>(5, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1));
    detail::native_sleep_until_exit(*impl_->native, left);
  }
}

void Process::abandon() { detail::native_abandon(*impl_->native); }

}  // namespace rdesk::platform
