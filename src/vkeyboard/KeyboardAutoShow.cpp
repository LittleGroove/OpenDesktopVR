// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/KeyboardAutoShow.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace OpenDesktopVR::VirtualKeyboard {

LONG AutoShow::VerticalDistance(const RECT& caret, POINT tap) noexcept {
  if (tap.y < caret.top) {
    return caret.top - tap.y;
  }
  if (tap.y > caret.bottom) {
    return tap.y - caret.bottom;
  }
  return 0;
}

bool AutoShow::IsNear(const RECT& caret, POINT tap) noexcept {
  const LONG height = std::max(LONG {0}, caret.bottom - caret.top);
  return VerticalDistance(caret, tap) <= std::max(NearPixels, NearCaretHeights * height);
}

void AutoShow::Tapped(double now, HWND window, POINT screen) noexcept {
  mWatched = window;
  mTap = Tap {now, window, screen};
  mLastTapTime = now;
}

std::optional<AutoShow::Change> AutoShow::Caret(double now, HWND window, bool typing, const std::optional<RECT>& caret) {
  if (!window || window != mWatched) {
    return std::nullopt;
  }
  if (typing && caret && mTap && mTap->mWindow == window) {
    const double since = now - mTap->mTime;
    if (since >= 0 && since <= TapWindowSeconds && IsNear(*caret, mTap->mScreen)) {
      const auto distance = VerticalDistance(*caret, mTap->mScreen);
      mTap.reset();// this press has done its job
      if (mMode == Mode::Hidden) {
        mMode = Mode::Auto;
        return Change {true, std::format("a caret {} px from the press, {:.0f} ms after it", distance, since * 1000.0)};
      }
      return std::nullopt;
    }
  }
  if (!typing && mMode == Mode::Auto) {
    if (mLastTapTime && now - *mLastTapTime < HideGraceSeconds) {
      return std::nullopt;// the 400 ms check decides
    }
    mMode = Mode::Hidden;
    return Change {false, "text focus left the window"};
  }
  return std::nullopt;
}

std::optional<AutoShow::Change> AutoShow::Enter(double) {
  if (mHideAfterEnter && mMode != Mode::Hidden) {
    mMode = Mode::Hidden;
    return Change {false, "Enter, with \"Hide after Enter\" on"};
  }
  return std::nullopt;
}

AutoShow::Change AutoShow::Toggle() {
  if (mMode == Mode::Hidden) {
    mMode = Mode::Manual;
    mTap.reset();
    return Change {true, "the keybind"};
  }
  mMode = Mode::Hidden;
  return Change {false, "the keybind"};
}

std::optional<AutoShow::Change> AutoShow::Dismiss(const char* why) {
  mTap.reset();
  if (mMode == Mode::Hidden) {
    return std::nullopt;
  }
  mMode = Mode::Hidden;
  return Change {false, why ? why : "dismissed"};
}

}// namespace OpenDesktopVR::VirtualKeyboard
