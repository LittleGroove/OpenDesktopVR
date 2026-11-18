// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The hands' pointer, into the captured windows (M4b): turns each touchable
// panel's input target into the injector's, applies the panel's switches
// ("Tap and drag with your finger" off: no presses; "Scroll with two
// fingers" off: no wheel), and writes the injector's stats line once a
// minute. Each press on a captured window is also told to a PressListener
// (the keyboard: where keys go, and whether a caret appears there; M5b).
// Called on the hand thread.
#pragma once

#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

namespace OpenDesktopVR::Host {

// Told about each finger press on a captured window, on the hand thread,
// after the press is queued. Must not block.
class PressListener {
 public:
  virtual ~PressListener() = default;
  virtual void Pressed(const TouchPanel& panel, const Pointer::Target& target, float u, float v) = 0;
};

// The injector's target for a touchable panel (nullopt: it takes no input)
std::optional<Pointer::Target> InjectorTarget(const TouchPanel& panel);

class InjectingSink final : public PointerSink {
 public:
  explicit InjectingSink(Pointer::Injector& injector, PressListener* listener = nullptr)
    : mInjector(injector), mListener(listener) {
  }

  void Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) override;

  // The stats since the last line, at exit (after the hand thread stopped),
  // so a session's last minute isn't lost (round 11)
  void LogLastStats();

 private:
  Pointer::Injector& mInjector;
  PressListener* mListener;
  // The panel a press is held on (0: none), to tell presses from moves
  uint64_t mPressedPanel {0};
  TimeUtil::PeriodicTimer mStats {std::chrono::seconds(60)};
};

}// namespace OpenDesktopVR::Host
