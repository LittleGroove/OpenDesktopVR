// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/QuickMenuModel.hpp>

namespace OpenDesktopVR::Host {

template <class Change>
void QuickMenuModel::Update(Change&& change) {
  Wake wake;
  {
    std::scoped_lock lock(mMutex);
    if (!change(mFrame)) {
      return;
    }
    ++mWakes;
    wake = mWake;
  }
  if (wake) {
    wake();
  }
}

void QuickMenuModel::SetWake(Wake wake) {
  std::scoped_lock lock(mMutex);
  mWake = std::move(wake);
}

void QuickMenuModel::SetLayout(std::shared_ptr<const QuickMenu::Layout> layout, bool hidden) {
  Update([&](QuickMenuFrame& frame) {
    const bool same = layout && frame.mLayout && *layout == *frame.mLayout;
    if (same && hidden == frame.mHidden) {
      return false;
    }
    if (!same) {
      // A new page: what the finger was over is found again on its next
      // sample (indices belong to a layout)
      frame.mLayout = std::move(layout);
      frame.mHover = -1;
      frame.mPressed = -1;
    }
    frame.mHidden = hidden;
    return true;
  });
}

void QuickMenuModel::SetPointer(const QuickMenu::Layout* layout, int hover, int pressed) {
  Update([&](QuickMenuFrame& frame) {
    if (layout != frame.mLayout.get() || (frame.mHover == hover && frame.mPressed == pressed)) {
      return false;
    }
    frame.mHover = hover;
    frame.mPressed = pressed;
    return true;
  });
}

std::shared_ptr<const QuickMenu::Layout> QuickMenuModel::Layout() const {
  std::scoped_lock lock(mMutex);
  return mFrame.mLayout;
}

QuickMenuFrame QuickMenuModel::Frame() const {
  std::scoped_lock lock(mMutex);
  return mFrame;
}

uint64_t QuickMenuModel::Wakes() const {
  std::scoped_lock lock(mMutex);
  return mWakes;
}

}// namespace OpenDesktopVR::Host
