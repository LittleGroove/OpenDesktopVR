// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// When the virtual keyboard shows and hides (doc 07 K3 to K6, VK-1, VK-2;
// ADR-036). Pure logic, with time passed in; the caller makes it
// thread-safe.
//
// - A finger press on a captured window: watch that window's caret, and
//   check again 80 and 400 ms later (VK-2).
// - Shown when the caret is within max(120 px, 4 x its height) of the
//   press, vertically, within 1.5 s of it (VK-1). An app focusing a field
//   by itself (no press near it) never shows it (K3).
// - Hidden when text focus leaves the window it types into (K4), but not
//   within 350 ms of a press: the 400 ms check decides, so moving from one
//   field to another doesn't flicker.
// - Enter checks again 150 ms later (VK-2); with "Hide after Enter" on, it
//   hides (K6).
// - The keybind shows or hides it (K5). Shown that way it stays until it's
//   dismissed: the keybind, the keyboard's hide key, hiding all panels.
#pragma once

#include <Windows.h>

#include <array>
#include <chrono>
#include <optional>
#include <string>

namespace OpenDesktopVR::VirtualKeyboard {

class AutoShow {
 public:
  enum class Mode : uint8_t { Hidden, Auto, Manual };

  struct Change {
    bool mShown {false};
    std::string mWhy;// for the log: never what was typed
  };

  // VK-1
  static constexpr double TapWindowSeconds = 1.5;
  static constexpr LONG NearPixels = 120;
  static constexpr LONG NearCaretHeights = 4;
  // VK-2
  static constexpr std::array<std::chrono::milliseconds, 2> TapChecks {
    std::chrono::milliseconds(80), std::chrono::milliseconds(400)};
  static constexpr std::chrono::milliseconds EnterCheck {150};
  // No hiding this soon after a press: the 400 ms check decides
  static constexpr double HideGraceSeconds = 0.35;

  // A finger press on a captured window, at this point on the screen
  // (physical pixels). The caller points the caret monitor at `window` and
  // asks it to check at TapChecks.
  void Tapped(double now, HWND window, POINT screen) noexcept;

  // The caret monitor's report for `window` (reports for a window no
  // longer watched are ignored)
  std::optional<Change> Caret(double now, HWND window, bool typing, const std::optional<RECT>& caret);

  // The keyboard's Enter. The caller asks the monitor to check at EnterCheck.
  std::optional<Change> Enter(double now);
  // The keybind (K5)
  Change Toggle();
  // The hide key, hiding all panels, the keyboard turned off
  std::optional<Change> Dismiss(const char* why);

  void SetHideAfterEnter(bool hide) noexcept {
    mHideAfterEnter = hide;
  }

  Mode GetMode() const noexcept {
    return mMode;
  }
  bool IsShown() const noexcept {
    return mMode != Mode::Hidden;
  }
  // The window being watched: the last one pressed
  HWND Watched() const noexcept {
    return mWatched;
  }

  // VK-1's distance: from the press to the caret, vertically (0 if the
  // press is level with it), and whether that's near enough
  static LONG VerticalDistance(const RECT& caret, POINT tap) noexcept;
  static bool IsNear(const RECT& caret, POINT tap) noexcept;

 private:
  struct Tap {
    double mTime {0};
    HWND mWindow {nullptr};
    POINT mScreen {};
  };

  Mode mMode {Mode::Hidden};
  HWND mWatched {nullptr};
  // The press that may still show the keyboard (cleared once it has)
  std::optional<Tap> mTap;
  std::optional<double> mLastTapTime;
  bool mHideAfterEnter {false};
};

}// namespace OpenDesktopVR::VirtualKeyboard
