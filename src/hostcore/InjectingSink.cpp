// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/InjectingSink.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

namespace OpenDesktopVR::Host {

std::optional<Pointer::Target> InjectorTarget(const TouchPanel& panel) {
  if (!panel.mTarget || !panel.mTarget->mWindow) {
    return std::nullopt;
  }
  const auto& shown = panel.mTarget->mShown;
  const auto& client = panel.mTarget->mClient;
  return Pointer::Target {
    static_cast<HWND>(panel.mTarget->mWindow),
    {static_cast<LONG>(shown.mLeft), static_cast<LONG>(shown.mTop), static_cast<LONG>(shown.mLeft + shown.mWidth),
      static_cast<LONG>(shown.mTop + shown.mHeight)},
    {static_cast<LONG>(client.mWidth), static_cast<LONG>(client.mHeight)},
    panel.mGeometry.mSize.x,
    panel.mGeometry.mSize.y,
  };
}

void InjectingSink::Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) {
  const auto target = panel ? InjectorTarget(*panel) : std::nullopt;
  if (!target) {
    // Gone, or content that takes no input: the injector lets go of it
    mInjector.Pointer(update.mPanelID, nullptr, update);
    mPressedPanel = 0;
  } else {
    auto filtered = update;
    if (!panel->mTouch && filtered.mState == HandInteraction::TouchState::Touching) {
      filtered.mState = HandInteraction::TouchState::Hovering;
    }
    if (!panel->mScroll) {
      filtered.mWheel = 0;
    }
    mInjector.Pointer(panel->mID, &*target, filtered);
    // A press (not a scroll, which sends no button): the keyboard's target
    const bool pressing = filtered.mState == HandInteraction::TouchState::Touching && filtered.mWheel == 0;
    if (pressing && mPressedPanel != panel->mID) {
      mPressedPanel = panel->mID;
      if (mListener) {
        ThreadUtil::CatchAll("press listener", [&] { mListener->Pressed(*panel, *target, update.mU, update.mV); });
      }
    } else if (filtered.mState != HandInteraction::TouchState::Touching) {
      mPressedPanel = 0;
    }
  }
  if (mStats.Due()) {
    mInjector.LogStats("Input (60 s)");
  }
}

void InjectingSink::LogLastStats() {
  mInjector.LogStats("Input (to exit)");
}

}// namespace OpenDesktopVR::Host
