// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// "Press the keys or buttons you want" (KY-4). Plain logic; the input
// thread feeds it and the settings window reads it, through a shared,
// locked session that outlives the dialog (InputThread).
//
// - Everything pressed together, up to four buttons, is the binding; it's
//   done when they're all let go.
// - Escape cancels (it can't be bound; the left mouse button isn't read).
// - Buttons already held when capture starts are ignored, and so are their
//   releases.
// - A keyboard modifier on its own isn't a binding: capture starts over and
//   says so.
#pragma once

#include <OpenDesktopVR/Keybinds.hpp>

#include <vector>

namespace OpenDesktopVR::Keybinds {

class BindCapture {
 public:
  enum class State { Waiting, Holding, Done, Cancelled };

  BindCapture() = default;
  // Buttons held when capture starts: they, and their releases, are ignored
  void Ignore(std::vector<Button> heldAtStart) {
    mIgnored = std::move(heldAtStart);
  }

  void OnButton(const Button& button, bool pressed);
  void Cancel() noexcept {
    mState = State::Cancelled;
  }

  State GetState() const noexcept {
    return mState;
  }
  // Done: the binding; Holding: what's pressed so far
  Binding Current() const {
    return Binding::From(mChord);
  }
  // The last try was a modifier on its own
  bool ModifierOnly() const noexcept {
    return mModifierOnly;
  }

 private:
  State mState {State::Waiting};
  std::vector<Button> mIgnored;
  std::vector<Button> mHeld;
  std::vector<Button> mChord;
  bool mModifierOnly {false};
};

}// namespace OpenDesktopVR::Keybinds
