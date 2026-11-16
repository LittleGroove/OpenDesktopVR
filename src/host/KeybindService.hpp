// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Keybinds in the host (doc 09, UI-2, UI-3): the bindings from settings go
// to the input thread (Keybinds::InputThread), and what fires comes back to
// the UI thread as a posted message (never SendMessage), where it shows or
// hides panels (PN-6) or changes a panel's brightness or tint (PN-5).
// Bindings are rebuilt whenever the panels or keybinds change.
#pragma once

#include "PanelManager.hpp"

#include <OpenDesktopVR/Events.hpp>
#include <OpenDesktopVR/InputThread.hpp>

#include <Windows.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::Host {

class KeybindService {
 public:
  // On the UI thread
  KeybindService(HINSTANCE instance, PanelManager& panels);
  ~KeybindService();
  KeybindService(const KeybindService&) = delete;
  KeybindService& operator=(const KeybindService&) = delete;

  std::shared_ptr<Keybinds::CaptureSession> StartCapture() {
    return mInput.StartCapture();
  }
  std::vector<Keybinds::ControllerInfo> Controllers() const {
    return mInput.Controllers();
  }
  bool KeyboardReady() const noexcept {
    return mInput.KeyboardReady();
  }

  // What a binding is already used for, other than `except` (conflicts)
  std::optional<Keybinds::Action> UsedBy(const Keybinds::Binding& binding, const Keybinds::Action& except) const;
  // "Show or hide all panels", "Notepad: Brighter"
  std::string Describe(const Keybinds::Action& action) const;

  // Runs an action now, as if its keybind fired (UI thread)
  void Run(const Keybinds::Action& action);

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  void Rebuild();

  HINSTANCE mInstance {nullptr};
  PanelManager& mPanels;
  // Message-only, on the UI thread: receives fired actions. Read by the
  // input thread, hence atomic.
  std::atomic<HWND> mWindow {nullptr};
  Keybinds::InputThread mInput;
  Events::Subscription mChanged;
};

}// namespace OpenDesktopVR::Host
