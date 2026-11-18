// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// For apphelper-test: a module that delay-loads user32, as Chromium does, so
// its IsWindowVisible goes through a delay-load import table.

#include <Windows.h>

extern "C" __declspec(dllexport) BOOL ProbeIsWindowVisible(HWND window) {
  return IsWindowVisible(window);
}
