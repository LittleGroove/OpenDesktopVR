// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Decides when a binding fires (KY-1), from button presses and releases.
// Plain logic: one owner (the input thread), no Windows calls.
//
// - A binding fires when one of its buttons is released while its other
//   buttons are still held. A modifier on its own never fires.
// - State is per device: each device's held buttons, and its "chord" (what
//   was pressed for the combination being made).
// - Switches and detents (ADR-028, amended after test round 7): a button
//   held for `LatchAfter` or more when another is pressed, or held since the
//   device was found, is "latched". It neither blocks a combination nor
//   starts one: when everything held is latched, the next press starts a
//   new chord. A binding that includes a latched button still needs it held.
//   Without this, a HOTAS with a toggle switch on, or a throttle in a
//   detent, never fired a binding (round 7).
// - A binding matches only what was pressed for it:
//   - on the keyboard, the modifiers (Ctrl, Shift, Alt, Windows) in the
//     chord are exactly its own, so F13 doesn't fire for Ctrl+F13 (maybe the
//     game's), but a movement key held for the game doesn't stop it. A
//     latched modifier still counts: Ctrl held for two seconds, then F13, is
//     Ctrl+F13;
//   - on a controller, the chord is within its buttons (a HOTAS "shift"
//     button plus B1, then B2, fires two bindings; a quick shift press is
//     part of the combination, a switch left on isn't);
//   - the binding with the most buttons wins.
// - A button held through a fire can't fire again when it's let go.
// - Clear() when the device list changes; Resync() when a lost device is
//   back (releases while it was away fire nothing; what's held is latched).
#pragma once

#include <OpenDesktopVR/Keybinds.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::Keybinds {

struct BoundAction {
  Binding mBinding;
  Action mAction;
};

class ChordTracker {
 public:
  // Held this long before another press: a switch or detent, not part of
  // the combination
  static constexpr std::chrono::milliseconds LatchAfter {1000};

  // Invalid bindings are left out. Held buttons are kept.
  void SetBindings(std::vector<BoundAction> bindings);

  // A button went down or up; the action whose binding fired, if any
  std::optional<Action> OnButton(const Button& button, bool pressed, TimeUtil::TimePoint now = TimeUtil::Now());

  // A controller binding that had all its buttons held when one was let go,
  // but didn't fire because of other buttons pressed with them. For the log
  // (controllers only: keyboard keys are never reported, ADR-028).
  struct Miss {
    Binding mBinding;
    std::vector<Button> mAlsoPressed;
  };
  std::optional<Miss> TakeMiss();

  // A device that was lost is back, or new, with these buttons held now
  void Resync(DeviceKind kind, const std::string& device, const std::vector<uint32_t>& held);
  // Devices changed: nothing is held any more
  void Clear();
  // Keyboard keys whose release was missed (the lock screen): dropped
  void DropKeys(const std::function<bool(uint32_t code)>& isUp);

  // Everything held now (a bind capture ignores these)
  std::vector<Button> Held() const;

 private:
  struct HeldButton {
    uint32_t mCode {0};
    TimeUtil::TimePoint mSince {};// the epoch: held since the device was found
    auto operator<=>(const HeldButton& other) const noexcept {
      return mCode <=> other.mCode;
    }
    bool operator==(const HeldButton& other) const noexcept {
      return mCode == other.mCode;
    }
  };
  struct Device {
    DeviceKind mKind {DeviceKind::Keyboard};
    std::string mDevice;
    std::string mProduct;
    std::string mName;
    std::vector<HeldButton> mHeld;     // sorted by code
    std::vector<uint32_t> mChord;  // sorted
    std::vector<uint32_t> mCarried;// held through a fire
  };
  Device& DeviceFor(const Button& button);
  Device* FindDevice(DeviceKind kind, const std::string& device);
  static bool IsHeld(const Device& device, uint32_t code);
  static bool IsLatched(const HeldButton& held, TimeUtil::TimePoint now);
  // A new chord: what's held and not latched (and, on the keyboard, every
  // held modifier)
  static void StartChord(Device& device, TimeUtil::TimePoint now);
  bool Matches(const Binding& binding, const Button& released, std::vector<Button>* alsoPressed);

  std::vector<BoundAction> mBindings;
  std::vector<Device> mDevices;
  std::optional<Miss> mMiss;
};

}// namespace OpenDesktopVR::Keybinds
