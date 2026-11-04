// Windows backend of rdesk::platform::Process. See process.hpp for the guarantees. This file can only be built and
// tested on Windows (CI job windows-msvc); keep it conservative.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "process_impl.hpp"
#include "rdesk/utf.hpp"

namespace rdesk::platform::detail {

struct Native {
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  HANDLE in_w = nullptr, out_r = nullptr, err_r = nullptr;  // the parent's ends
  DWORD pid = 0;
  bool killed = false;
  std::mutex m;  // guards in_w and job
};

void NativeDeleter::operator()(Native* n) const noexcept {
  if (!n) return;
  for (HANDLE h : {n->in_w, n->out_r, n->err_r, n->process, n->job}) {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  delete n;
}

namespace {

std::string display(const std::filesystem::path& p) {
  auto u = wide_to_utf8(p.wstring());  // path::string() would go through the ANSI code page and mangle non-ASCII names
  return u ? u.value() : std::string("<path>");
}

Error spawn_error(const ProcessOptions& o, const std::string& why) {
  return make_error(Errc::BackendUnavailable, "cannot start '" + display(o.exe) + "': " + why);
}

std::string last_error_text(DWORD code) {
  wchar_t* buf = nullptr;
  const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                 reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::string text = "error " + std::to_string(code);
  if (n > 0 && buf != nullptr) {
    auto u8 = wide_to_utf8(std::wstring_view(buf, n));
    if (u8) text = u8.value() + " (" + std::to_string(code) + ")";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  }
  if (buf) LocalFree(buf);
  return text;
}

// The inverse of CommandLineToArgvW / the MS C runtime's parsing rules.
void append_quoted(std::wstring& out, const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    out += arg;
    return;
  }
  out += L'"';
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');  // backslashes before the closing quote are doubled
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');  // and so are those before a literal quote, plus the escape
      out += L'"';
    } else {
      out.append(backslashes, L'\\');
      out += arg[i];
    }
  }
  out += L'"';
}

struct PipeEnds {
  HANDLE parent = nullptr;
  HANDLE child = nullptr;
};

// parent_reads: the parent keeps the read end (child's stdout / stderr); otherwise the parent keeps the write end.
bool make_pipe(PipeEnds& p, bool parent_reads) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;  // inheritable; the parent's end is switched off below
  HANDLE r = nullptr, w = nullptr;
  if (!CreatePipe(&r, &w, &sa, 0)) return false;
  p.parent = parent_reads ? r : w;
  p.child = parent_reads ? w : r;
  if (!SetHandleInformation(p.parent, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(r);
    CloseHandle(w);
    p = {};
    return false;
  }
  return true;
}

void close_handle(HANDLE& h) {
  if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  h = nullptr;
}

}  // namespace

Result<NativePtr> native_spawn(const ProcessOptions& o) {
  const std::wstring exe_w = o.exe.wstring();
  std::wstring cmd;
  append_quoted(cmd, exe_w);
  for (const auto& a : o.args) {
    auto w = utf8_to_wide(a);
    if (!w) return spawn_error(o, "an argument is not valid UTF-8");
    cmd += L' ';
    append_quoted(cmd, w.value());
  }
  cmd.push_back(L'\0');  // CreateProcessW may modify the buffer

  PipeEnds in, out, err;
  if (!make_pipe(in, /*parent_reads=*/false) || !make_pipe(out, true) || !make_pipe(err, true)) {
    const DWORD e = GetLastError();
    for (HANDLE* h : {&in.parent, &in.child, &out.parent, &out.child, &err.parent, &err.child}) close_handle(*h);
    return spawn_error(o, "cannot create pipes: " + last_error_text(e));
  }
  auto cleanup_all = [&] {
    for (HANDLE* h : {&in.parent, &in.child, &out.parent, &out.child, &err.parent, &err.child}) close_handle(*h);
  };

  // Only these three handles may be inherited by the child.
  HANDLE inherit[3] = {in.child, out.child, err.child};
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<char> attr_storage(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
  if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
    const DWORD e = GetLastError();
    cleanup_all();
    return spawn_error(o, "InitializeProcThreadAttributeList: " + last_error_text(e));
  }
  if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr)) {
    const DWORD e = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    cleanup_all();
    return spawn_error(o, "UpdateProcThreadAttribute: " + last_error_text(e));
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in.child;
  si.StartupInfo.hStdOutput = out.child;
  si.StartupInfo.hStdError = err.child;
  si.lpAttributeList = attrs;

  std::wstring cwd_w;
  if (!o.cwd.empty()) cwd_w = o.cwd.wstring();

  PROCESS_INFORMATION pi{};
  const BOOL created = CreateProcessW(exe_w.c_str(), cmd.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
                                      CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                      cwd_w.empty() ? nullptr : cwd_w.c_str(), &si.StartupInfo, &pi);
  const DWORD create_error = created ? 0 : GetLastError();
  DeleteProcThreadAttributeList(attrs);
  if (!created) {
    cleanup_all();
    return spawn_error(o, last_error_text(create_error));
  }

  // The child is suspended: put it in a kill-on-close job before it can run or start anything.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  bool ok = job != nullptr;
  if (ok) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) != FALSE;
  }
  if (ok) ok = AssignProcessToJobObject(job, pi.hProcess) != FALSE;
  if (!ok) {
    const DWORD e = GetLastError();
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    cleanup_all();
    return spawn_error(o, "cannot put the process in a job object: " + last_error_text(e));
  }
  if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
    const DWORD e = GetLastError();
    TerminateJobObject(job, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    cleanup_all();
    return spawn_error(o, "ResumeThread: " + last_error_text(e));
  }
  CloseHandle(pi.hThread);
  close_handle(in.child);
  close_handle(out.child);
  close_handle(err.child);

  NativePtr n(new Native());
  n->process = pi.hProcess;
  n->job = job;
  n->pid = pi.dwProcessId;
  n->in_w = in.parent;
  n->out_r = out.parent;
  n->err_r = err.parent;
  return n;
}

std::int64_t native_pid(const Native& n) noexcept { return static_cast<std::int64_t>(n.pid); }

long native_read(Native& n, int stream, char* buf, std::size_t len) noexcept {
  HANDLE h = stream == 1 ? n.out_r : n.err_r;
  DWORD got = 0;
  if (!ReadFile(h, buf, static_cast<DWORD>(len > 0x10000 ? 0x10000 : len), &got, nullptr)) {
    return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
  }
  return static_cast<long>(got);
}

bool native_write_stdin(Native& n, const char* data, std::size_t len) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  if (n.in_w == nullptr) return false;
  while (len > 0) {
    DWORD put = 0;
    if (!WriteFile(n.in_w, data, static_cast<DWORD>(len > 0x10000 ? 0x10000 : len), &put, nullptr)) return false;
    data += put;
    len -= put;
  }
  return true;
}

void native_close_stdin(Native& n) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  close_handle(n.in_w);
}

void native_kill(Native& n) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  if (n.job != nullptr) TerminateJobObject(n.job, 1);        // the child and everything it started
  else if (n.process != nullptr) TerminateProcess(n.process, 1);  // job already abandoned
}

bool native_poll_exit(Native& n, ExitStatus& out) noexcept {
  if (WaitForSingleObject(n.process, 0) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  GetExitCodeProcess(n.process, &code);
  out = ExitStatus{};
  out.raw = code;
  // NTSTATUS error codes (0xC0000005 access violation, 0xC00000FD stack overflow, ...) have both top bits set.
  if ((code & 0xC0000000u) == 0xC0000000u) {
    out.crashed = true;
  } else {
    out.exited_normally = true;
    out.code = static_cast<int>(code);
  }
  return true;
}

void native_sleep_until_exit(Native& n, int max_ms) noexcept { WaitForSingleObject(n.process, static_cast<DWORD>(max_ms)); }

void native_abandon(Native& n) noexcept {
  std::lock_guard<std::mutex> lock(n.m);
  close_handle(n.job);  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ends the child
}

}  // namespace rdesk::platform::detail
