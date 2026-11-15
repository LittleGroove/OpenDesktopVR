// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Window helpers for picking and capturing desktop windows (doc 04 "Window
// capture"): which windows can be picked, what they're called, and where
// their client area sits inside a captured frame. None of them throw.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::WindowUtil {

struct WindowInfo {
  HWND mWindow {nullptr};
  std::wstring mTitle;
  std::wstring mClassName;
  std::wstring mExecutablePath;// full path; empty if the process can't be opened
  DWORD mProcessID {0};
};

std::optional<WindowInfo> Describe(HWND window) noexcept;

// Visible, titled, top-level windows a user may want in VR: not tool
// windows, not cloaked (UWP background windows), not our own process's.
// Sorted by title.
std::vector<WindowInfo> ListCaptureCandidates() noexcept;

bool IsCloaked(HWND window) noexcept;

// The client area, in the coordinates of a captured frame of this window.
// WGC frames cover the extended frame bounds (DWMWA_EXTENDED_FRAME_BOUNDS);
// the client area is GetClientRect mapped to the screen. Clamped to the
// frame; nullopt if the window is gone or has no client area.
std::optional<RECT> ClientCropInFrame(HWND window, uint32_t frameWidth, uint32_t frameHeight) noexcept;

// The file name part of the executable, for menus ("Discord.exe")
std::wstring ExecutableName(const WindowInfo& info);

}// namespace OpenDesktopVR::WindowUtil
