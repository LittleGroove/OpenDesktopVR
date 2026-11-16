// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// A game controller's buttons and hat directions from one read to the next
// (KY-1, KY-2): which were pressed and which let go.
//
// Some reads are a baseline instead: what's held is taken as held from the
// start, and nothing counts as pressed or let go.
//
// - **After the device is found, or acquired again:** every read for
//   SettleTime is a baseline. Right after Acquire, DirectInput reports
//   nothing held until the device's first report arrives, and a device can
//   send odd reports while it starts. Test round 8: a HOTAS's switches read
//   as fresh presses, and a replugged stick's first report briefly showed a
//   hat pushed.
// - **After the device list changes:** the next read is a baseline. The
//   device is already reporting, so there is no settling.

#pragma once

#include <OpenDesktopVR/TimeUtil.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace OpenDesktopVR::Keybinds {

class ControllerState {
 public:
  static constexpr std::chrono::milliseconds SettleTime {500};

  struct Changes {
    // This read was a baseline: Held() is held from the start
    bool mBaseline {false};
    // The baseline is final (settled), so worth reporting
    bool mSettled {false};
    std::vector<uint32_t> mReleased;
    std::vector<uint32_t> mPressed;
  };

  // Found, or acquired again: baseline reads until now + SettleTime
  void Restart(TimeUtil::TimePoint now);
  // The device list changed: the next read is a baseline
  void Resync();
  // `held`: the buttons and hat codes held now, sorted
  Changes Update(std::vector<uint32_t> held, TimeUtil::TimePoint now);

  // When a read is due to finish settling; nullopt when not settling
  std::optional<TimeUtil::TimePoint> SettleDue() const;
  const std::vector<uint32_t>& Held() const {
    return mHeld;
  }

 private:
  std::vector<uint32_t> mHeld;// sorted
  TimeUtil::TimePoint mSettleUntil {};
  bool mResync {true};
  bool mSettling {false};
};

}// namespace OpenDesktopVR::Keybinds
