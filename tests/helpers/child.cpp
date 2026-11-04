// A small program the process-layer tests start. Modes (first argument):
//   args               print every following argument on its own line (UTF-8), then exit 0
//   echo               copy stdin lines to stdout, flushing each, until EOF; then print "eof" and exit 0
//   eof                read stdin until EOF, then exit 17
//   sleep MS           sleep, then exit 0
//   crash              die from a crash: SIGABRT (POSIX) or an access violation (Windows)
//   stderr TEXT        write TEXT to stderr and exit 3
//   partial            write "ab" and, after a pause, "cd\n" (one line delivered in two pieces)
//   flood N            write N lines to stdout without waiting for the reader
//   exit CODE          exit with CODE
//   fds                print the numbers of the open file descriptors (POSIX only; the Windows test uses `handle`)
//   handle VALUE       print "open" if the handle VALUE (decimal) is valid in this process, else "closed" (Windows)
//   grandchild MS      start `sleep MS` as a child of its own and print its pid, then sleep too
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "rdesk/utf.hpp"
#else
#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

static void sleep_ms(long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static int run(const std::vector<std::string>& a, const char* self) {
  (void)self;  // only the POSIX `grandchild` mode needs it
  if (a.empty()) return 64;
  const std::string& mode = a[0];
  if (mode == "args") {
    for (std::size_t i = 1; i < a.size(); ++i) std::printf("%s\n", a[i].c_str());
    std::fflush(stdout);
    return 0;
  }
  if (mode == "echo") {
    std::string line;
    while (std::getline(std::cin, line)) {
      std::printf("%s\n", line.c_str());
      std::fflush(stdout);
    }
    std::printf("eof\n");
    std::fflush(stdout);
    return 0;
  }
  if (mode == "eof") {
    std::string line;
    while (std::getline(std::cin, line)) {
    }
    return 17;
  }
  if (mode == "sleep") {
    sleep_ms(a.size() > 1 ? std::atol(a[1].c_str()) : 1000);
    return 0;
  }
  if (mode == "crash") {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    *static_cast<volatile int*>(nullptr) = 1;
    return 0;
#else
    std::abort();  // a real SIGABRT even under ASan (which would turn a null write into exit code 1)
#endif
  }
  if (mode == "stderr") {
    std::fprintf(stderr, "%s", a.size() > 1 ? a[1].c_str() : "");
    std::fflush(stderr);
    return 3;
  }
  if (mode == "partial") {
    std::printf("ab");
    std::fflush(stdout);
    sleep_ms(150);
    std::printf("cd\n");
    std::fflush(stdout);
    return 0;
  }
  if (mode == "flood") {
    const long n = a.size() > 1 ? std::atol(a[1].c_str()) : 1000;
    for (long i = 0; i < n; ++i) std::printf("line %ld\n", i);
    std::fflush(stdout);
    return 0;
  }
  if (mode == "exit") return a.size() > 1 ? std::atoi(a[1].c_str()) : 0;
#ifndef _WIN32
  if (mode == "fds") {
    DIR* d = opendir("/proc/self/fd");
    const int self_fd = d ? dirfd(d) : -1;
    std::vector<int> fds;
    while (d) {
      const dirent* e = readdir(d);
      if (!e) break;
      if (e->d_name[0] == '.') continue;
      const int fd = std::atoi(e->d_name);
      if (fd != self_fd) fds.push_back(fd);
    }
    if (d) closedir(d);
    for (const int fd : fds) std::printf("%d\n", fd);
    std::fflush(stdout);
    return 0;
  }
  if (mode == "grandchild") {
    const long ms = a.size() > 1 ? std::atol(a[1].c_str()) : 30000;
    const std::string ms_s = std::to_string(ms);
    char* argv[] = {const_cast<char*>(self), const_cast<char*>("sleep"), const_cast<char*>(ms_s.c_str()), nullptr};
    pid_t pid = 0;
    // The grandchild inherits our stdout, so the parent's pipe stays open until it is killed too.
    if (posix_spawn(&pid, self, nullptr, nullptr, argv, environ) != 0) return 65;
    std::printf("%d\n", static_cast<int>(pid));
    std::fflush(stdout);
    sleep_ms(ms);
    return 0;
  }
#else
  if (mode == "handle") {
    const auto value = static_cast<unsigned long long>(std::strtoull(a.size() > 1 ? a[1].c_str() : "0", nullptr, 10));
    DWORD flags = 0;
    const bool open = GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value)), &flags) != 0;
    std::printf("%s\n", open ? "open" : "closed");
    std::fflush(stdout);
    return 0;
  }
  if (mode == "grandchild") {
    const long ms = a.size() > 1 ? std::atol(a[1].c_str()) : 30000;
    // Start ourselves in `sleep` mode, inheriting stdout.
    wchar_t path[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof path / sizeof path[0]));
    std::wstring line = L"\"" + std::wstring(path) + L"\" sleep " + std::to_wstring(ms);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 65;
    std::printf("%lu\n", pi.dwProcessId);
    std::fflush(stdout);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    sleep_ms(ms);
    return 0;
  }
#endif
  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 64;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    auto u = rdesk::platform::wide_to_utf8(argv[i]);
    args.push_back(u ? u.value() : std::string("<invalid utf-16>"));
  }
  char self[1] = {0};
  return run(args, self);
}
#else
int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return run(args, argv[0]);
}
#endif
