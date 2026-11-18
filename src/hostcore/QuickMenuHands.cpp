// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/QuickMenuHands.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

namespace OpenDesktopVR::Host {

QuickMenuHands::QuickMenuHands(std::shared_ptr<QuickMenuModel> model) : mModel(std::move(model)) {
}

void QuickMenuHands::SetNotify(std::function<void()> notify) {
  std::scoped_lock lock(mMutex);
  mNotify = std::move(notify);
}

void QuickMenuHands::Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) {
  const auto layout = mModel->Layout();
  if (!layout || layout->mWidth == 0 || layout->mHeight == 0) {
    mPointer.Reset();
    return;
  }
  using HandInteraction::TouchState;
  auto touch = QuickMenu::Touch::Away;
  if (panel) {
    if (update.mState == TouchState::Touching) {
      touch = QuickMenu::Touch::Down;
    } else if (update.mState == TouchState::Hovering) {
      touch = QuickMenu::Touch::Hover;
    }
  }
  const auto result = mPointer.Update(*layout, update.mU, update.mV, touch, update.mWheel);
  mModel->SetPointer(layout.get(), result.mHover, result.mPressed);
  if (result.mActivated) {
    Push(*result.mActivated);
  }
  if (result.mScrollRows != 0) {
    Push({QuickMenu::CommandKind::Scroll, 0, result.mScrollRows});
  }
}

void QuickMenuHands::Push(const QuickMenu::Command& command) {
  std::function<void()> notify;
  {
    std::scoped_lock lock(mMutex);
    if (command.mKind == QuickMenu::CommandKind::Scroll && mCount > 0) {
      // A scroll merges with one already waiting at the back
      auto& last = mQueue[(mHead + mCount - 1) % QueueCapacity];
      if (last.mKind == QuickMenu::CommandKind::Scroll) {
        last.mStep += command.mStep;
        return;
      }
    }
    if (mCount == QueueCapacity) {
      if (mDropped.fetch_add(1, std::memory_order_relaxed) == 0) {
        Log::Warning("Quick menu: the UI thread isn't taking taps; dropping them");
      }
      return;
    }
    mQueue[(mHead + mCount) % QueueCapacity] = command;
    ++mCount;
    notify = mNotify;
  }
  if (notify) {
    ThreadUtil::CatchAll("quick menu notify", notify);
  }
}

std::optional<QuickMenu::Command> QuickMenuHands::TakeCommand() {
  std::scoped_lock lock(mMutex);
  if (mCount == 0) {
    return std::nullopt;
  }
  const auto command = mQueue[mHead];
  mHead = (mHead + 1) % QueueCapacity;
  --mCount;
  return command;
}

}// namespace OpenDesktopVR::Host
