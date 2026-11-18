// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/ControllerState.hpp>

#include <algorithm>
#include <iterator>

namespace OpenDesktopVR::Keybinds {

void ControllerState::Restart(TimeUtil::TimePoint now) {
  mResync = true;
  mSettling = true;
  mSettleUntil = now + SettleTime;
}

void ControllerState::Resync() {
  mResync = true;
}

ControllerState::Changes ControllerState::Update(std::vector<uint32_t> held, TimeUtil::TimePoint now) {
  Changes changes;
  if (mResync || mSettling) {
    mResync = false;
    mHeld = std::move(held);
    changes.mBaseline = true;
    if (mSettling && now < mSettleUntil) {
      return changes;// still settling: the next read is a baseline too
    }
    mSettling = false;
    changes.mSettled = true;
    return changes;
  }
  // Releases first, so a quick change of buttons isn't read as a combination
  std::ranges::set_difference(mHeld, held, std::back_inserter(changes.mReleased));
  std::ranges::set_difference(held, mHeld, std::back_inserter(changes.mPressed));
  mHeld = std::move(held);
  return changes;
}

std::optional<TimeUtil::TimePoint> ControllerState::SettleDue() const {
  if (!mSettling) {
    return std::nullopt;
  }
  return mSettleUntil;
}

}// namespace OpenDesktopVR::Keybinds
