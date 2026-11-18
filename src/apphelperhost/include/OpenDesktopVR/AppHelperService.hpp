// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Loads the app helper into captured apps (doc 06; ADR-031), for two
// things, each asked for with its own lease:
// - keeping an app painting while the game covers it (CT-3, "Occlusion");
// - input (M4b): the fakes that let the injector's events land as if the
//   app were in front, without it coming in front.
//
// - **When:** a window panel's capture asks at capture start (HK-1) and lets
//   go when its capture stops, through a lease. Painting: only apps that
//   need it (AppHelperRules: Chromium-based and Firefox). Input: any app,
//   for panels with touch or scroll on. Only windows the user picked, never
//   the game or the host itself (HK-11), never an elevated app while we
//   aren't, never a 32-bit app (ADR-015). Each panel can turn either off.
// - **How:** one set of hooks per UI thread (WH_GETMESSAGE for posted
//   commands, WH_CALLWNDPROC for the injector's sent ones), shared by every
//   window of that thread and both kinds of lease (HK-2). Commands are
//   posted only after the hooks are in,
//   starting with Hello, and every one is answered (HK-4). When the last
//   window lets go, Unload waits for the helper to finish (HK-9) before the
//   hook is removed, which unloads it.
// - **Threads:** everything runs on the service's own thread. Callers only
//   queue a request, so a lease never blocks (render thread), and the host
//   never waits on another process. Replies come back as posted messages to
//   the service's window.
#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace OpenDesktopVR::AppHelper {

class AppHelperService {
 public:
  struct Options {
    // The helper DLL; empty: next to the host's executable
    std::filesystem::path mHelperPath;
    // How long the app may take to answer Hello (it may be busy starting)
    std::chrono::milliseconds mHelloTimeout {5000};
    // ... and Unload
    std::chrono::milliseconds mUnloadTimeout {2000};
  };

  enum class State {
    Waiting,    // asked; the helper isn't confirmed yet
    On,         // the helper keeps it painting
    NotNeeded,  // this kind of app doesn't stop painting when covered
    Unavailable,// it would need it, but it can't have it (elevated, 32-bit, the game)
    Failed,     // it should have it, but loading failed or the app didn't answer
  };

  struct Status {
    State mState {State::Waiting};
    // For the settings window: "On: the helper is loaded in firefox.exe."
    std::string mText;
  };

  AppHelperService();
  explicit AppHelperService(Options options);
  ~AppHelperService();
  AppHelperService(const AppHelperService&) = delete;
  AppHelperService& operator=(const AppHelperService&) = delete;

  // The game's process (the one our OpenXR layer is in): never helped
  // (HK-11). Any thread.
  void SetGameProcess(std::function<uint32_t()> gameProcess);

  // While the lease lives, the window's app keeps painting when covered, if
  // it needs to and can. Any thread; never blocks. `panelID` names it for
  // StatusOf.
  [[nodiscard]] std::shared_ptr<void> KeepPainting(uint64_t panelID, HWND window);
  // While the lease lives, the helper's input fakes are set up in the
  // window's app (they act only around injected events). Any thread.
  [[nodiscard]] std::shared_ptr<void> Input(uint64_t panelID, HWND window);

  // The newest lease's status for a panel; nullopt if it has none. Any thread.
  std::optional<Status> StatusOf(uint64_t panelID) const;
  std::optional<Status> InputStatusOf(uint64_t panelID) const;

  // For tests
  std::size_t Installs() const;
  // Replies that answer nothing sent (a command handled twice: HK-3)
  uint64_t UnexpectedReplies() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> mImpl;
  std::shared_ptr<void> MakeLease(uint64_t panelID, HWND window, int kind);
};

}// namespace OpenDesktopVR::AppHelper
