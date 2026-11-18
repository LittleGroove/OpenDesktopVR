// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Works out whether a window (usually not the foreground window) has a text
// field focused, i.e. whether typing would go somewhere, from the caret:
//
// - Chromium-based apps (Edge, Chrome, Discord, Spotify...) have an
//   accessibility caret object that's visible when an editable field has focus
//   and raises caret events; this doesn't turn on their full accessibility
//   support
// - Win32 text controls, and Firefox (once it has been asked for its caret
//   object, which this does), create a real Windows caret
//
// Caret events say when to look again; the state comes from asking. 'Not
// typing' only counts once it has lasted a moment, as carets are hidden and
// recreated while text changes or pages load.
#pragma once

#include <Windows.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace OpenDesktopVR {

class TextInputMonitor final {
 public:
  struct State {
    // A text field has focus
    bool mTyping {false};
    // The caret, in screen coordinates, if known
    std::optional<RECT> mCaret;
    bool operator==(const State&) const;
  };
  // Called on the monitor's own thread whenever the state or caret changes
  using Callback = std::function<void(HWND window, const State&)>;

  explicit TextInputMonitor(Callback);
  ~TextInputMonitor();
  TextInputMonitor(const TextInputMonitor&) = delete;
  TextInputMonitor& operator=(const TextInputMonitor&) = delete;

  // The captured top-level window to watch; nullptr stops watching.
  void SetWindow(HWND topLevel);
  // Check again after a delay, e.g. after a tap or a key. With `report`, the
  // callback is called even if nothing changed.
  void Poke(
    std::chrono::milliseconds delay = std::chrono::milliseconds(60),
    bool report = false);

  State GetState() const;

  // How long 'not typing' must last before it counts
  static constexpr auto NotTypingDelay = std::chrono::milliseconds(250);

  // Asks the window directly; may wait for the window's thread, so don't call
  // this from a UI thread
  static State Query(HWND topLevel);

 private:
  class Impl;
  std::unique_ptr<Impl> mImpl;
};

}// namespace OpenDesktopVR
