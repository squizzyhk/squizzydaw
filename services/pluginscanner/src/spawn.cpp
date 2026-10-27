// pluginscanner: run a child process with a timeout (kill on timeout). Windows + POSIX.
#include "spawn.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#endif

#include "sq/platform.hpp"

namespace sq::pscan {

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
// CommandLineToArgvW-compatible quoting.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
  if (!cmd.empty()) cmd += L' ';
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }
  cmd += L'"';
  for (size_t i = 0;; ++i) {
    size_t bs = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++bs;
    }
    if (i == arg.size()) {
      cmd.append(bs * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      cmd.append(bs * 2 + 1, L'\\');
      cmd += L'"';
    } else {
      cmd.append(bs, L'\\');
      cmd += arg[i];
    }
  }
  cmd += L'"';
}
}  // namespace

SpawnResult runProcess(const std::vector<std::string>& argv, uint32_t timeoutMs) {
  SpawnResult r;
  if (argv.empty()) return r;
  std::wstring cmd;
  for (const auto& a : argv) appendQuoted(cmd, widen(a));
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  // No error dialogs from a crashing child (WER / "program stopped working").
  UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  SetErrorMode(oldMode);
  if (!ok) return r;
  r.started = true;
  DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
  if (w == WAIT_TIMEOUT) {
    r.timedOut = true;
    TerminateProcess(pi.hProcess, 0xDEAD);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  r.exitCode = static_cast<int64_t>(code);
  // NTSTATUS-style exception codes (0xC0000005 access violation ...) mean the child crashed.
  r.crashed = !r.timedOut && (code & 0xC0000000u) == 0xC0000000u;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return r;
}
#else
SpawnResult runProcess(const std::vector<std::string>& argv, uint32_t timeoutMs) {
  SpawnResult r;
  if (argv.empty()) return r;
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) return r;
  if (pid == 0) {
    execv(args[0], args.data());
    _exit(127);
  }
  r.started = true;
  auto start = std::chrono::steady_clock::now();
  int status = 0;
  for (;;) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) {
      r.timedOut = true;
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!r.timedOut) {
    if (WIFEXITED(status)) r.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) {
      r.crashed = true;
      r.exitCode = -WTERMSIG(status);
    }
  }
  return r;
}
#endif

}  // namespace sq::pscan
