// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Runs a command beside a private host instance, for end-to-end tests that
// need the real host app (the simulator's hand scenarios).
//
//   with-host [--also <exe> <file>] <host exe> [host args...] -- <command> [args...]
//
// --also: starts another program first (click-target with its log file;
//   the file's folder is made), and stops it at the end.
//
// - A private instance name (OPENDESKTOPVR_INSTANCE), so a real host on the
//   same PC is never touched; the host then uses its built-in test panels
//   and saves nothing.
// - The layer's own test panel is off (OPENDESKTOPVR_TEST_PANEL=0): only
//   the host's panels show.
// - The host gets --no-register --warp --exit-after 180 before its own args.
// - Exits with the command's exit code; the host is stopped either way.

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace {

std::wstring Quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring quoted = L"\"";
  for (const auto c: arg) {
    if (c == L'"') {
      quoted += L'\\';
    }
    quoted += c;
  }
  return quoted + L"\"";
}

HANDLE Start(const std::wstring& exe, const std::vector<std::wstring>& args) {
  std::wstring command = Quote(exe);
  for (const auto& arg: args) {
    command += L" " + Quote(arg);
  }
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &info)) {
    std::wprintf(L"FAIL: couldn't start %s (%lu)\n", exe.c_str(), GetLastError());
    return nullptr;
  }
  CloseHandle(info.hThread);
  return info.hProcess;
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  std::vector<std::wstring> hostArgs {L"--no-register", L"--warp", L"--exit-after", L"180"};
  std::wstring hostExe;
  std::wstring commandExe;
  std::vector<std::wstring> commandArgs;
  std::wstring alsoExe;
  std::wstring alsoFile;
  bool inCommand = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg {argv[i]};
    if (!inCommand && hostExe.empty() && arg == L"--also" && i + 2 < argc) {
      alsoExe = argv[++i];
      alsoFile = argv[++i];
    } else if (!inCommand && arg == L"--") {
      inCommand = true;
    } else if (!inCommand && hostExe.empty()) {
      hostExe = arg;
    } else if (!inCommand) {
      hostArgs.push_back(arg);
    } else if (commandExe.empty()) {
      commandExe = arg;
    } else {
      commandArgs.push_back(arg);
    }
  }
  if (hostExe.empty() || commandExe.empty()) {
    std::printf("Usage: with-host <host exe> [host args...] -- <command> [args...]\n");
    return 2;
  }
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", std::format(L"with-host-{}", GetCurrentProcessId()).c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_TEST_PANEL", L"0");

  HANDLE also = nullptr;
  if (!alsoExe.empty()) {
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path {alsoFile}.parent_path(), ignored);
    std::filesystem::remove(alsoFile, ignored);
    also = Start(alsoExe, {alsoFile});
    if (!also) {
      return 1;
    }
    Sleep(1000);// its window is up before the host looks for it
  }
  HANDLE host = Start(hostExe, hostArgs);
  if (!host) {
    if (also) {
      TerminateProcess(also, 0);
      CloseHandle(also);
    }
    return 1;
  }
  int exitCode = 1;
  if (HANDLE command = Start(commandExe, commandArgs)) {
    if (WaitForSingleObject(command, 170'000) == WAIT_TIMEOUT) {
      std::printf("FAIL: the command took more than 170 s\n");
      TerminateProcess(command, 1);
      WaitForSingleObject(command, 5000);
    } else {
      DWORD code = 1;
      GetExitCodeProcess(command, &code);
      exitCode = static_cast<int>(code);
    }
    CloseHandle(command);
  }
  if (WaitForSingleObject(host, 0) != WAIT_TIMEOUT) {
    std::printf("FAIL: the host exited during the test\n");
    exitCode = exitCode == 0 ? 1 : exitCode;
  }
  TerminateProcess(host, 0);
  WaitForSingleObject(host, 5000);
  CloseHandle(host);
  if (also) {
    TerminateProcess(also, 0);
    WaitForSingleObject(also, 5000);
    CloseHandle(also);
  }
  return exitCode;
}
