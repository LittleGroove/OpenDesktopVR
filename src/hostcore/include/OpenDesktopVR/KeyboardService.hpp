// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The virtual keyboard on the host's side (doc 07; ADR-036):
// - typing (TypingSink, on the hand thread): presses on the keyboard
//   panel become key strokes for the window of the last finger press on a
//   captured panel (PressListener), through the injector, which never
//   waits; held keys repeat; each press clicks and flashes;
// - showing and hiding (KeyboardAutoShow): the caret monitor's reports (on
//   its own thread), the keybind (UI thread), the hide key and Enter (hand
//   thread);
// - what the keyboard panel draws (KeyboardModel): legends, states, the
//   target's name and the caret for the preview strip;
// - a stats line a minute: counts only, never which keys or where (they'd
//   record passwords).
// One lock guards it all; nothing that waits is ever called under it.
#pragma once

#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/InjectingSink.hpp>
#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/KeyboardModel.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>

#include <Windows.h>

#include <functional>
#include <memory>
#include <optional>

namespace OpenDesktopVR::Host {

class KeyboardService final : public TypingSink, public PressListener {
 public:
  // `click` plays the key click at a volume (0 to 1); it must not wait
  KeyboardService(Pointer::Injector& injector, std::shared_ptr<VirtualKeyboard::KeyboardModel> model,
    std::function<void(float volume)> click);
  ~KeyboardService() override;
  KeyboardService(const KeyboardService&) = delete;
  KeyboardService& operator=(const KeyboardService&) = delete;

  // UI thread: loads the layouts (K8), then applies everything
  void SetSettings(const KeyboardSettings& settings);
  // Any thread: the keybind (K5); true if it now shows
  bool Toggle();
  void Dismiss(const char* why);
  bool IsShown() const;

  // TypingSink and PressListener: the hand thread
  void Typing(const TouchPanel& keyboard, const HandInteraction::TypingResult& result, bool appeared) override;
  void TypingStopped() override;
  void Tick() override;
  void Pressed(const TouchPanel& panel, const Pointer::Target& target, float u, float v) override;

  // At exit, after the hand thread stopped
  void LogLastStats();

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

// The caret (in the window's own client units, the client area
// `clientWidth` x `clientHeight` of them) in the panel's content pixels,
// for the preview strip: the inverse of Pointer::ClientPoint
RECT CaretInContent(const Pointer::Target& target, const RECT& caretClient, LONG clientWidth, LONG clientHeight) noexcept;

}// namespace OpenDesktopVR::Host
