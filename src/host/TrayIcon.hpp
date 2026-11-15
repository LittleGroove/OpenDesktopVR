// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The host's tray icon and its menu, and the UI thread's message loop.
// The UI thread never waits on the render thread: it only flips flags and
// asks for a frame.
#pragma once

#include <OpenDesktopVR/RenderLoop.hpp>

#include <Windows.h>

namespace OpenDesktopVR::Host {

class TrayIcon {
 public:
  TrayIcon(HINSTANCE instance, RenderLoop& renderer);
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  void QuitAfter(double seconds);
  // Runs the message loop until Exit; returns the exit code
  int Run();

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
  void ShowMenu();
  void AddIcon();

  HINSTANCE mInstance {nullptr};
  RenderLoop& mRenderer;
  HWND mWindow {nullptr};
  UINT mTaskbarCreated {0};
  bool mIconAdded {false};
};

}// namespace OpenDesktopVR::Host
