#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "process_impl.hpp"

extern char** environ;

namespace rdesk::platform::detail {

struct Native {
  pid_t pid = -1;
  int in_fd = -1, out_fd = -1, err_fd = -1;  // the parent's ends
  bool reaped = false;
  int status = 0;
  std::mutex m;  // guards in_fd (close vs write) and the reaping
};

void NativeDeleter::operator()(Native* n) const noexcept {
  if (!n) return;
  for (const int fd : {n->in_fd, n->out_fd, n->err_fd}) {
    if (fd >= 0) ::close(fd);
  }
  delete n;
}

namespace {

Error spawn_error(const ProcessOptions& o, const std::string& why) {
  return make_error(Errc::BackendUnavailable, "cannot start '" + o.exe.string() + "': " + why);
}

struct Pipe {
  int r = -1, w = -1;
};

bool make_pipe(Pipe& p) {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) return false;
  p.r = fds[0];
  p.w = fds[1];
  return true;
}

void close_if(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

}  // namespace

Result<NativePtr> native_spawn(const ProcessOptions& o) {
  static std::once_flag once;
  std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });

  Pipe in, out, err;
  auto cleanup = [&] {
    for (int* fd : {&in.r, &in.w, &out.r, &out.w, &err.r, &err.w}) close_if(*fd);
  };
  if (!make_pipe(in) || !make_pipe(out) || !make_pipe(err)) {
    const std::string why = std::strerror(errno);
    cleanup();
    return spawn_error(o, "cannot create pipes: " + why);
  }

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawnattr_init(&attr);
  auto destroy = [&] {
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
  };

  // dup2 clears close-on-exec on the target descriptor, so 0/1/2 survive the exec; all pipe originals are CLOEXEC.
  int rc = posix_spawn_file_actions_adddup2(&fa, in.r, 0);
  if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, out.w, 1);
  if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, err.w, 2);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
  // Nothing but 0, 1, 2 may leak into the child, whatever the rest of the parent has open.
  if (rc == 0) rc = posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
  if (!o.cwd.empty()) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    if (rc == 0) rc = posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());
#endif
  }
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);  // we ignore SIGPIPE; the child gets the default back
  sigset_t none;
  sigemptyset(&none);
  if (rc == 0) rc = posix_spawnattr_setsigdefault(&attr, &defaults);
  if (rc == 0) rc = posix_spawnattr_setsigmask(&attr, &none);
  if (rc == 0) rc = posix_spawnattr_setpgroup(&attr, 0);  // own process group: kill(-pgid) reaches grandchildren
  if (rc == 0) rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
  if (rc != 0) {
    destroy();
    cleanup();
    return spawn_error(o, std::strerror(rc));
  }

  const std::string exe = o.exe.string();
  std::vector<std::string> storage;
  storage.push_back(exe);
  for (const auto& a : o.args) storage.push_back(a);
  std::vector<char*> argv;
  for (auto& s : storage) argv.push_back(s.data());
  argv.push_back(nullptr);

  pid_t pid = -1;
  rc = posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
  destroy();
  if (rc != 0) {
    cleanup();
    return spawn_error(o, std::strerror(rc));
  }
  close_if(in.r);
  close_if(out.w);
  close_if(err.w);

  NativePtr n(new Native());
  n->pid = pid;
  n->in_fd = in.w;
  n->out_fd = out.r;
  n->err_fd = err.r;
  return n;
}

std::int64_t native_pid(const Native& n) noexcept { return n.pid; }

long native_read(Native& n, int stream, char* buf, std::size_t len) noexcept {
  const int fd = stream == 1 ? n.out_fd : n.err_fd;
  for (;;) {
    const ssize_t got = ::read(fd, buf, len);
    if (got >= 0) return static_cast<long>(got);
    if (errno != EINTR) return -1;
  }
}

bool native_write_stdin(Native& n, const char* data, std::size_t len) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  if (n.in_fd < 0) return false;
  while (len > 0) {
    const ssize_t put = ::write(n.in_fd, data, len);
    if (put < 0) {
      if (errno == EINTR) continue;
      return false;  // EPIPE: the child closed its stdin or exited
    }
    data += put;
    len -= static_cast<std::size_t>(put);
  }
  return true;
}

void native_close_stdin(Native& n) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  close_if(n.in_fd);
}

void native_kill(Native& n) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  if (n.reaped || n.pid <= 0) return;
  ::kill(-n.pid, SIGKILL);  // the process group (pgid == pid)
  ::kill(n.pid, SIGKILL);
}

bool native_poll_exit(Native& n, ExitStatus& out) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  if (!n.reaped) {
    int status = 0;
    pid_t r;
    do {
      r = ::waitpid(n.pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == 0) return false;
    n.reaped = true;
    n.status = r < 0 ? 0 : status;
  }
  out = ExitStatus{};
  out.raw = static_cast<std::uint32_t>(n.status);
  if (WIFEXITED(n.status)) {
    out.exited_normally = true;
    out.code = WEXITSTATUS(n.status);
  } else if (WIFSIGNALED(n.status)) {
    out.crashed = true;
    out.signal = WTERMSIG(n.status);
  }
  return true;
}

void native_sleep_until_exit(Native&, int max_ms) noexcept {
  timespec ts{};
  ts.tv_nsec = 1000000L;  // 1 ms: there is no timed waitpid
  for (int i = 0; i < max_ms; ++i) ::nanosleep(&ts, nullptr);
}

void native_abandon(Native&) noexcept {}

}  // namespace rdesk::platform::detail
