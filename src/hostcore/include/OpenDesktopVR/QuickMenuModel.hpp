// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// What the quick menu panel draws (ADR-041), shared between the threads
// that change it and the render thread that draws it:
// - the UI thread's controller sets the layout (pages, panels, values) and
//   whether the panel shows at all (closed with its button off: hidden);
// - the hand thread sets which target the finger is over and which it
//   holds, so a press shows at once without waiting for the UI thread.
// A change wakes the renderer only if it alters the picture: an identical
// layout, or the finger moving within one target, doesn't (RN-6).
#pragma once

#include <OpenDesktopVR/QuickMenu.hpp>

#include <functional>
#include <memory>
#include <mutex>

namespace OpenDesktopVR::Host {

struct QuickMenuFrame {
  std::shared_ptr<const QuickMenu::Layout> mLayout;// null until the first
  int mHover {-1};
  int mPressed {-1};
  // Until the controller's first layout: nothing to show yet, so the layer
  // isn't given an empty quad at startup
  bool mHidden {true};
};

class QuickMenuModel {
 public:
  using Wake = std::function<void()>;

  // The menu's content source, while it exists (render thread)
  void SetWake(Wake wake);

  // UI thread
  void SetLayout(std::shared_ptr<const QuickMenu::Layout> layout, bool hidden);
  // Hand thread: indices into `layout`, ignored if the layout has changed
  // since (they'd point at the wrong targets)
  void SetPointer(const QuickMenu::Layout* layout, int hover, int pressed);

  std::shared_ptr<const QuickMenu::Layout> Layout() const;
  QuickMenuFrame Frame() const;
  // How many times a change woke the renderer (tests: RN-6)
  uint64_t Wakes() const;

 private:
  // Called with the lock held; true if it changed something drawn. Wakes
  // after the lock is released.
  template <class Change>
  void Update(Change&& change);

  mutable std::mutex mMutex;
  QuickMenuFrame mFrame;
  Wake mWake;
  uint64_t mWakes {0};
};

}// namespace OpenDesktopVR::Host
