// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/BindCapture.hpp>

#include <Windows.h>

#include <algorithm>

namespace OpenDesktopVR::Keybinds {

void BindCapture::OnButton(const Button& button, bool pressed) {
  if (mState == State::Done || mState == State::Cancelled) {
    return;
  }
  if (button.mKind == DeviceKind::Keyboard && (button.mCode & 0xFF) == VK_ESCAPE) {
    if (pressed) {
      mState = State::Cancelled;
    }
    return;
  }
  if (const auto ignored = std::ranges::find(mIgnored, button); ignored != mIgnored.end()) {
    if (!pressed) {
      mIgnored.erase(ignored);// let go: from now on it counts
    }
    return;
  }
  if (pressed) {
    if (std::ranges::find(mHeld, button) == mHeld.end()) {
      mHeld.push_back(button);
      if (mChord.size() < MaxButtons && std::ranges::find(mChord, button) == mChord.end()) {
        mChord.push_back(button);
      }
      mState = State::Holding;
    }
    return;
  }
  if (std::erase(mHeld, button) == 0 || !mHeld.empty()) {
    return;
  }
  // All let go: that's the binding, unless it was only modifiers
  if (Binding::From(mChord).IsValid()) {
    mModifierOnly = false;
    mState = State::Done;
  } else {
    mModifierOnly = true;
    mChord.clear();
    mState = State::Waiting;
  }
}

}// namespace OpenDesktopVR::Keybinds
