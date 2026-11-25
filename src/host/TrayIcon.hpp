// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The host's tray icon and its menu, and the UI thread's message loop.
// The UI thread never waits on the render thread: it only flips flags and
// asks for a frame. Left-click opens the settings window; right-click lists
// the windows on a worker thread (ADR-020), then shows the menu.
#pragma once

#include "PanelManager.hpp"
#include "SettingsWindow.hpp"

#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <Windows.h>

#include <string_view>
#include <vector>

namespace OpenDesktopVR::Host {

class TrayIcon {
 public:
  TrayIcon(HINSTANCE instance, RenderLoop& renderer, PanelManager& panels, SettingsWindow& settings);
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  void QuitAfter(double seconds);
  // A notification from the tray icon (Windows shows it as a toast).
  // Clicking it opens the settings window on the tab that explains it
  enum class Notice {
    Administrator,// a warning (Q9): the Diagnostics tab
    Updated,// an update by itself (UP-8): the General tab
  };
  void ShowNotice(Notice notice, std::wstring_view title, std::wstring_view text);
  // Runs the message loop until Exit; returns the exit code
  int Run();

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
  void RequestMenu();
  void ShowMenu(std::vector<WindowUtil::WindowInfo> windows);
  void AddIcon();

  HINSTANCE mInstance {nullptr};
  RenderLoop& mRenderer;
  PanelManager& mPanels;
  SettingsWindow& mSettings;
  ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>> mListing;
  // What the open menu listed, so a command maps back to it
  std::vector<WindowUtil::WindowInfo> mMenuWindows;
  std::vector<uint64_t> mMenuPanels;
  std::vector<std::string> mMenuProfiles;
  HWND mWindow {nullptr};
  UINT mTaskbarCreated {0};
  bool mIconAdded {false};
  Notice mNotice {Notice::Administrator};// the last one shown
};

}// namespace OpenDesktopVR::Host
