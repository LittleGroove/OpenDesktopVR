// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Crash reports for the host and tools (doc 09 "Debug bundle"; TH-10).
//
// An unhandled exception, std::terminate, a pure virtual call or an invalid
// CRT parameter writes a minidump, `<program>-<date>-<time>-<pid>.dmp`, to
// the dump folder, appends a line to `crashes.txt` there, and ends the
// process.
//
// None of it depends on the rest of the program still working (TH-10):
// - no logging thread, no heap after Install, no locks;
// - the file names are built in fixed buffers.
//
// Window procedures run inside Windows' own callbacks, where an exception
// otherwise surfaces only as 0xC000041D with its origin lost (test round
// 15). Guarded() runs one with the crash handling around it, so its report
// shows where it happened.
//
// Not for the OpenXR layer: a game's process isn't ours to set crash
// handling for.
#pragma once

#include <Windows.h>

#include <filesystem>
#include <string_view>

namespace OpenDesktopVR::CrashHandler {

// Once, early in the program
void Install(const std::filesystem::path& dumpFolder, std::wstring_view programName) noexcept;

// Writes the report for an exception; returns EXCEPTION_EXECUTE_HANDLER.
// Use it as an __except filter.
LONG Report(EXCEPTION_POINTERS* exception) noexcept;

using WindowProcedure = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
// Runs a window procedure; a crash in it is reported, then the process ends.
// Before Install, a crash goes on as if it weren't there.
LRESULT Guarded(WindowProcedure procedure, HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

// A window procedure for a window class, with Guarded() around it:
//   windowClass.lpfnWndProc = &CrashHandler::GuardedProcedure<&MyWindow::WindowProc>;
template <WindowProcedure Procedure>
LRESULT CALLBACK GuardedProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
  return Guarded(Procedure, window, message, wParam, lParam);
}

}// namespace OpenDesktopVR::CrashHandler
