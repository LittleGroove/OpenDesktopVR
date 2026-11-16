// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Test helper: makes one of the test's windows the real foreground window
// (like a game in front of the apps), whatever started the test.
#pragma once

#include <Windows.h>

namespace OpenDesktopVR::Testing {

// True if `window` is the foreground window afterwards
inline bool TakeForeground(HWND window) {
  SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  const HWND foreground = GetForegroundWindow();
  const DWORD theirs = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  const bool shared = theirs != 0 && theirs != GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(), theirs, TRUE);
  SetForegroundWindow(window);
  if (shared) {
    AttachThreadInput(GetCurrentThreadId(), theirs, FALSE);
  }
  return GetForegroundWindow() == window;
}

}// namespace OpenDesktopVR::Testing
