// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The quick menu under the fingers (ADR-041), on the hand thread:
// - the menu panel's pointer runs through QuickMenu::MenuPointer against
//   the layout now shown; hover and presses go straight to the model, so a
//   press shows without waiting for the UI thread;
// - a finished tap (or a two-finger scroll) becomes a command, queued for
//   the UI thread's controller, which is told with `notify` (a posted
//   message: never waits). The queue holds 32; taps come at human speed,
//   so it only fills if the UI thread is stuck, and then the newest is
//   dropped and counted rather than the hand thread waiting (TH-5).
// Scrolls are merged; taps never are.
#pragma once

#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/QuickMenuModel.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace OpenDesktopVR::Host {

class QuickMenuHands final : public PointerSink {
 public:
  static constexpr std::size_t QueueCapacity = 32;

  explicit QuickMenuHands(std::shared_ptr<QuickMenuModel> model);

  // Before the hand thread starts; any thread
  void SetNotify(std::function<void()> notify);

  // PointerSink: the hand thread
  void Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) override;

  // UI thread: the next command, oldest first
  std::optional<QuickMenu::Command> TakeCommand();
  uint64_t Dropped() const noexcept {
    return mDropped.load(std::memory_order_relaxed);
  }

 private:
  void Push(const QuickMenu::Command& command);

  std::shared_ptr<QuickMenuModel> mModel;
  QuickMenu::MenuPointer mPointer;// hand thread only
  std::mutex mMutex;
  std::array<QuickMenu::Command, QueueCapacity> mQueue {};
  std::size_t mHead {0};
  std::size_t mCount {0};
  std::function<void()> mNotify;
  std::atomic<uint64_t> mDropped {0};
};

// Sends the quick menu's pointer to the menu and every other panel's on
// (the injector). By the update's panel ID, so leaving the menu for a
// window still reaches the menu.
class PointerRouter final : public PointerSink {
 public:
  PointerRouter(PointerSink& panels, PointerSink& menu, uint64_t menuID) : mPanels(panels), mMenu(menu), mMenuID(menuID) {
  }
  void Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) override {
    (update.mPanelID == mMenuID ? mMenu : mPanels).Pointer(panel, update);
  }

 private:
  PointerSink& mPanels;
  PointerSink& mMenu;
  uint64_t mMenuID;
};

}// namespace OpenDesktopVR::Host
