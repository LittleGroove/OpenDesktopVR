// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/InjectingSink.hpp>

namespace OpenDesktopVR::Host {

void InjectingSink::Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) {
  if (!panel || !panel->mTarget || !panel->mTarget->mWindow) {
    // Gone, or content that takes no input: the injector lets go of it
    mInjector.Pointer(update.mPanelID, nullptr, update);
  } else {
    auto filtered = update;
    if (!panel->mTouch && filtered.mState == HandInteraction::TouchState::Touching) {
      filtered.mState = HandInteraction::TouchState::Hovering;
    }
    if (!panel->mScroll) {
      filtered.mWheel = 0;
    }
    const auto& shown = panel->mTarget->mShown;
    const auto& client = panel->mTarget->mClient;
    const Pointer::Target target {
      static_cast<HWND>(panel->mTarget->mWindow),
      {static_cast<LONG>(shown.mLeft), static_cast<LONG>(shown.mTop), static_cast<LONG>(shown.mLeft + shown.mWidth),
        static_cast<LONG>(shown.mTop + shown.mHeight)},
      {static_cast<LONG>(client.mWidth), static_cast<LONG>(client.mHeight)},
    };
    mInjector.Pointer(panel->mID, &target, filtered);
  }
  if (mStats.Due()) {
    mInjector.LogStats("Input (60 s)");
  }
}

}// namespace OpenDesktopVR::Host
