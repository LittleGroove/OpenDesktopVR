// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Crash reports (M6c, TH-10): child copies of this program crash on
// purpose, with the crash handling installed, in a temporary folder:
// - an access violation on the main thread;
// - an access violation inside a window procedure run through Guarded()
//   (test round 15's crash was one: without it, only 0xC000041D);
// - std::terminate.
// Each must leave a minidump and a line in crashes.txt naming the exception.
//
//   crash-test

#include "check.hpp"

#include <OpenDesktopVR/CrashHandler.hpp>

#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

using namespace OpenDesktopVR;
using Testing::Check;
using Testing::Scenario;
namespace fs = std::filesystem;

namespace {

LRESULT CALLBACK CrashingProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_USER) {
    volatile int* nothing = nullptr;
    *nothing = 1;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK GuardedProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  return CrashHandler::Guarded(&CrashingProcedure, window, message, wParam, lParam);
}

int Child(const std::string& mode, const fs::path& folder) {
  SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
  CrashHandler::Install(folder, L"crash-test");
  if (mode == "access") {
    volatile int* nothing = nullptr;
    *nothing = 1;
  } else if (mode == "window") {
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = &GuardedProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OdvrCrashTest";
    RegisterClassW(&windowClass);
    const HWND window = CreateWindowExW(0, L"OdvrCrashTest", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    SendMessageW(window, WM_USER, 0, 0);
  } else if (mode == "terminate") {
    std::terminate();
  }
  return 0;// not reached
}

struct Outcome {
  DWORD mExitCode {0};
  std::size_t mDumpBytes {0};
  std::string mLine;
};

Outcome RunChild(const std::string& mode, const fs::path& folder) {
  wchar_t self[MAX_PATH] {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  auto command = std::format(L"\"{}\" --child {} \"{}\"", self, std::wstring(mode.begin(), mode.end()), folder.wstring());
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION process {};
  Outcome outcome;
  if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    return outcome;
  }
  WaitForSingleObject(process.hProcess, 30'000);
  GetExitCodeProcess(process.hProcess, &outcome.mExitCode);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  std::error_code error;
  for (const auto& entry: fs::directory_iterator(folder, error)) {
    if (entry.path().extension() == L".dmp") {
      outcome.mDumpBytes = static_cast<std::size_t>(entry.file_size(error));
    }
  }
  std::ifstream log {folder / L"crashes.txt"};
  std::getline(log, outcome.mLine);
  return outcome;
}

}// namespace

int main(int argc, char** argv) {
  if (argc >= 4 && std::string {argv[1]} == "--child") {
    return Child(argv[2], argv[3]);
  }
  Testing::TempFolder temp;
  const struct {
    const char* mMode;
    const char* mWhat;
    const char* mCode;
  } cases[] {
    {"access", "an access violation", "0xC0000005"},
    {"window", "an access violation in a window procedure (Guarded)", "0xC0000005"},
    {"terminate", "std::terminate", "0xE0D5C001"},
  };
  for (const auto& test: cases) {
    Scenario(std::format("A crash report for {} (TH-10)", test.mWhat).c_str(), [&] {
      const fs::path folder = fs::path {temp.Path()} / test.mMode;
      const auto outcome = RunChild(test.mMode, folder);
      Check(outcome.mExitCode != 0, std::format("the process ended (exit code 0x{:08X})", outcome.mExitCode));
      Check(outcome.mDumpBytes > 1024, std::format("a minidump was written ({} bytes)", outcome.mDumpBytes));
      Check(outcome.mLine.find("crashed: exception") != std::string::npos && outcome.mLine.find(test.mCode) != std::string::npos,
        std::format("crashes.txt names it ({})", outcome.mLine));
    });
  }
  return Testing::Finish();
}
