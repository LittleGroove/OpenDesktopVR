// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The quick menu's controller (doc 09 "In-VR UI", CT-10, UI-6; ADR-041),
// on the UI thread, beside the panel list it changes:
// - gathers what the menu shows (panels, profiles, settings) and lays out
//   the current page whenever something changes, here or elsewhere (a
//   keybind, the settings window), into the shared model;
// - runs the commands the hand thread queues (a posted message: the hand
//   thread never waits), each addressed by the panel's stable ID and
//   ignored if that panel has gone (DU-2);
// - opens and closes the menu: its button, its keybind, its close button;
// - the Windows page's list is made on a worker (window enumeration stays
//   off the UI thread, ADR-020), checked every 100 ms until it's there;
// - the Position page (test round 19) moves a panel, or the keyboard, which
//   shows while its Position page is open so you can see where it goes.
// Every message is caught (TH-8).
#pragma once

#include "PanelManager.hpp"

#include <OpenDesktopVR/Events.hpp>
#include <OpenDesktopVR/QuickMenu.hpp>
#include <OpenDesktopVR/QuickMenuHands.hpp>
#include <OpenDesktopVR/QuickMenuModel.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace OpenDesktopVR::Host {

class QuickMenuController {
 public:
  QuickMenuController(HINSTANCE instance, PanelManager& panels, RenderLoop& renderer, std::shared_ptr<QuickMenuModel> model,
    QuickMenuHands& hands);
  ~QuickMenuController();
  QuickMenuController(const QuickMenuController&) = delete;
  QuickMenuController& operator=(const QuickMenuController&) = delete;

  // The keybind (UI thread): true if it's now open
  bool Toggle(const char* why);
  void Open(const char* why);
  void Close(const char* why);
  bool IsOpen() const noexcept {
    return mState.mOpen;
  }
  // Runs a command as if it were tapped (UI thread; tests and tools)
  void Run(const QuickMenu::Command& command);

  // Shows (true) or hides (false) the keyboard while its Position page is
  // open; showing returns false if it was already showing (then it's left
  // showing afterwards)
  void SetKeyboardPreview(std::function<bool(bool show)> preview) {
    mKeyboardPreview = std::move(preview);
  }

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  void TakeCommands();
  void RunOnPanel(const QuickMenu::Command& command);
  void RunSetting(const QuickMenu::Command& command);
  void RunPosition(const QuickMenu::Command& command);
  void PreviewKeyboard();
  void Rebuild();
  QuickMenu::MenuData Gather() const;
  void StartListing();
  void CheckListing();

  HINSTANCE mInstance {nullptr};
  PanelManager& mPanels;
  RenderLoop& mRenderer;
  std::shared_ptr<QuickMenuModel> mModel;
  QuickMenuHands& mHands;
  // Message-only, on the UI thread; read by the hand thread's notify
  std::atomic<HWND> mWindow {nullptr};
  QuickMenu::MenuState mState;
  // What the page shows now: commands' indices (profiles, windows) are
  // into these
  QuickMenu::MenuData mData;
  ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>> mListing;
  std::vector<WindowUtil::WindowInfo> mWindows;
  bool mHaveWindows {false};
  Events::Subscription mChanged;
  bool mRebuilding {false};
  std::function<bool(bool)> mKeyboardPreview;
  bool mPreviewing {false};    // the keyboard's Position page is open
  bool mPreviewShowed {false}; // and showed the keyboard
};

}// namespace OpenDesktopVR::Host
