// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Decides when a binding fires (KY-1), from button presses and releases.
// Plain logic: one owner (the input thread), no Windows calls.
//
// - A binding fires when one of its buttons is released while its other
//   buttons are still held. A modifier on its own never fires.
// - State is per device: each device's held buttons, and its "chord" (every
//   button pressed since it was last idle, or since a binding fired).
// - A binding matches only what was pressed for it:
//   - on the keyboard, its keys were pressed and the modifiers (Ctrl,
//     Shift, Alt, Windows) are exactly its own, so F13 doesn't fire for
//     Ctrl+F13 (maybe the game's), but a movement key held for the game
//     doesn't stop it;
//   - on a controller, exactly its buttons were pressed (a HOTAS "shift"
//     button plus B1, then B2, fires two bindings);
//   - the binding with the most buttons wins.
// - A button held through a fire can't fire again when it's let go.
// - Clear() when the device list changes; Resync() when a lost device is
//   back (releases while it was away fire nothing).
#pragma once

#include <OpenDesktopVR/Keybinds.hpp>

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
  // Invalid bindings are left out. Held buttons are kept.
  void SetBindings(std::vector<BoundAction> bindings);

  // A button went down or up; the action whose binding fired, if any
  std::optional<Action> OnButton(const Button& button, bool pressed);

  // A device that was lost is back, with these buttons held now
  void Resync(DeviceKind kind, const std::string& device, const std::vector<uint32_t>& held);
  // Devices changed: nothing is held any more
  void Clear();
  // Keyboard keys whose release was missed (the lock screen): dropped
  void DropKeys(const std::function<bool(uint32_t code)>& isUp);

  // Everything held now (a bind capture ignores these)
  std::vector<Button> Held() const;

 private:
  struct Device {
    DeviceKind mKind {DeviceKind::Keyboard};
    std::string mDevice;
    std::string mProduct;
    std::string mName;
    std::vector<uint32_t> mHeld;   // sorted
    std::vector<uint32_t> mChord;  // sorted
    std::vector<uint32_t> mCarried;// held through a fire
  };
  Device& DeviceFor(const Button& button);
  Device* FindDevice(DeviceKind kind, const std::string& device);
  bool Matches(const Binding& binding, const Button& released);

  std::vector<BoundAction> mBindings;
  std::vector<Device> mDevices;
};

}// namespace OpenDesktopVR::Keybinds
