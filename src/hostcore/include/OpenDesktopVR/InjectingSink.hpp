// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The hands' pointer, into the captured windows (M4b): turns each touchable
// panel's input target into the injector's, applies the panel's switches
// ("Tap and drag with your finger" off: no presses; "Scroll with two
// fingers" off: no wheel), and writes the injector's stats line once a
// minute. Called on the hand thread.
#pragma once

#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

namespace OpenDesktopVR::Host {

class InjectingSink final : public PointerSink {
 public:
  explicit InjectingSink(Pointer::Injector& injector) : mInjector(injector) {
  }

  void Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) override;

  // The stats since the last line, at exit (after the hand thread stopped),
  // so a session's last minute isn't lost (round 11)
  void LogLastStats();

 private:
  Pointer::Injector& mInjector;
  TimeUtil::PeriodicTimer mStats {std::chrono::seconds(60)};
};

}// namespace OpenDesktopVR::Host
