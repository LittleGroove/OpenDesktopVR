// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Commands from Stream Deck, VoiceAttack and other tools, in the host (UI-9,
// doc 09 "Automation"; ADR-042):
// - the command line (`OpenDesktopVR.exe toggle Discord`) finds this
//   message-only window by its class (per instance, like the host's kernel
//   objects) and sends its words with WM_COPYDATA, waiting at most 3 s;
// - here, on the UI thread, the words are checked as untrusted (TH-9:
//   bounded, never throwing) and the panels they name are found. The reply
//   (done, refused, busy) goes back at once; the command is queued (64 at
//   most) and run from a posted message, as keybinds are, by panel ID, a
//   panel gone by then being skipped (DU-2);
// - every command is logged, and refusals with the reason, at most 10 a
//   minute (doc 11: "logged and dropped; no crash");
// - "window" finds the window on a worker (enumeration stays off the UI
//   thread, ADR-020).
#pragma once

#include "PanelManager.hpp"
#include "QuickMenuController.hpp"

#include <OpenDesktopVR/Automation.hpp>
#include <OpenDesktopVR/KeyboardService.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <Windows.h>

#include <chrono>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace OpenDesktopVR::Host {

// The automation window's class: the command line finds the host by it
std::wstring AutomationWindowClass();

// The command line's side: sends the words to the running host and
// returns its reply as the program's exit code (Automation::Reply). "help"
// shows the commands instead.
int SendCommand(std::span<const std::wstring> words);

class AutomationService {
 public:
  static constexpr std::size_t QueueCapacity = 64;

  AutomationService(HINSTANCE instance, PanelManager& panels, RenderLoop& renderer, KeyboardService& keyboard,
    QuickMenuController& quickMenu);
  ~AutomationService();
  AutomationService(const AutomationService&) = delete;
  AutomationService& operator=(const AutomationService&) = delete;

  // The settings window's "Try it": the words run now, on the UI thread;
  // what happened, in a sentence
  std::string Try(std::span<const std::wstring> words);
  // "settings": opens the settings window
  void SetSettingsOpener(std::function<void()> open) {
    mOpenSettings = std::move(open);
  }

 private:
  struct Ready {
    Automation::Command mCommand;
    std::vector<uint64_t> mPanels;// found when it arrived
    std::string mProfileID;       // "profile <name>"
  };

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  Automation::Reply Receive(const COPYDATASTRUCT& data);
  // Checks a command against what's there now; nullopt with `why` if not
  std::optional<Ready> Prepare(const Automation::Command& command, std::string& why) const;
  void RunQueued();
  // Runs it (its repeats too); what happened
  std::string Run(const Ready& ready);
  std::string RunOnce(const Ready& ready);
  std::string Nudge(const Ready& ready);
  void Refused(const std::string& why);
  void StartListing();
  void CheckListing();

  HINSTANCE mInstance {nullptr};
  PanelManager& mPanels;
  RenderLoop& mRenderer;
  KeyboardService& mKeyboard;
  QuickMenuController& mQuickMenu;
  std::function<void()> mOpenSettings;
  std::wstring mClass;
  HWND mWindow {nullptr};
  std::deque<Ready> mQueue;
  bool mRunPosted {false};
  // "window": the panels and the title text waiting for the list
  struct WindowRequest {
    std::vector<uint64_t> mPanels;
    std::wstring mText;
  };
  std::vector<WindowRequest> mWindowRequests;
  ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>> mListing;
  // Refusals logged this minute, and those left out
  std::chrono::steady_clock::time_point mRefusalMinute {};
  int mRefusalsLogged {0};
  int mRefusalsQuiet {0};
};

}// namespace OpenDesktopVR::Host
