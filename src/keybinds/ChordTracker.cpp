// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/ChordTracker.hpp>

#include <algorithm>

namespace OpenDesktopVR::Keybinds {

namespace {

bool Contains(const std::vector<uint32_t>& sorted, uint32_t code) {
  return std::ranges::binary_search(sorted, code);
}

void Insert(std::vector<uint32_t>& sorted, uint32_t code) {
  const auto at = std::ranges::lower_bound(sorted, code);
  if (at == sorted.end() || *at != code) {
    sorted.insert(at, code);
  }
}

void Erase(std::vector<uint32_t>& sorted, uint32_t code) {
  if (const auto at = std::ranges::lower_bound(sorted, code); at != sorted.end() && *at == code) {
    sorted.erase(at);
  }
}

bool IsModifierCode(DeviceKind kind, uint32_t code) {
  return IsModifier(Button {kind, {}, code});
}

}// namespace

void ChordTracker::SetBindings(std::vector<BoundAction> bindings) {
  std::erase_if(bindings, [](const BoundAction& bound) { return !bound.mBinding.IsValid(); });
  mBindings = std::move(bindings);
}

ChordTracker::Device& ChordTracker::DeviceFor(const Button& button) {
  if (auto* device = FindDevice(button.mKind, button.mDevice)) {
    return *device;
  }
  Device device;
  device.mKind = button.mKind;
  device.mDevice = button.mDevice;
  device.mProduct = button.mProduct;
  device.mName = button.mDeviceName;
  return mDevices.emplace_back(std::move(device));
}

ChordTracker::Device* ChordTracker::FindDevice(DeviceKind kind, const std::string& device) {
  const auto found = std::ranges::find_if(mDevices, [&](const Device& d) { return d.mKind == kind && d.mDevice == device; });
  return found == mDevices.end() ? nullptr : &*found;
}

bool ChordTracker::IsHeld(const Device& device, uint32_t code) {
  return std::ranges::binary_search(device.mHeld, code, {}, &HeldButton::mCode);
}

bool ChordTracker::IsLatched(const HeldButton& held, TimeUtil::TimePoint now) {
  return held.mSince == TimeUtil::TimePoint {} || now - held.mSince >= LatchAfter;
}

void ChordTracker::StartChord(Device& device, TimeUtil::TimePoint now) {
  device.mChord.clear();
  for (const auto& held: device.mHeld) {
    const bool modifier = device.mKind == DeviceKind::Keyboard && IsModifierCode(DeviceKind::Keyboard, held.mCode);
    if (modifier || !IsLatched(held, now)) {
      Insert(device.mChord, held.mCode);
    }
  }
}

bool ChordTracker::Matches(const Binding& binding, const Button& released, std::vector<Button>* alsoPressed) {
  if (std::ranges::find(binding.mButtons, released) == binding.mButtons.end()) {
    return false;
  }
  // Its other buttons are still held (latched ones too)
  for (const auto& button: binding.mButtons) {
    const auto* device = FindDevice(button.mKind, button.mDevice);
    if (!device || !IsHeld(*device, button.mCode)) {
      return false;
    }
  }
  // And what was pressed for this combination on each of its devices was
  // pressed for it
  bool matches = true;
  for (const auto& button: binding.mButtons) {
    const auto* device = FindDevice(button.mKind, button.mDevice);
    std::vector<uint32_t> own;
    for (const auto& b: binding.mButtons) {
      if (b.mKind == button.mKind && b.mDevice == button.mDevice) {
        Insert(own, b.mCode);
      }
    }
    for (const auto code: device->mChord) {
      if (Contains(own, code)) {
        continue;
      }
      if (device->mKind == DeviceKind::Keyboard) {
        if (IsModifierCode(DeviceKind::Keyboard, code)) {
          matches = false;// another modifier: another combination
        }
        continue;// other keys (held for the game) don't matter
      }
      matches = false;
      if (alsoPressed) {
        const Button extra {device->mKind, device->mDevice, code, device->mProduct, device->mName};
        if (std::ranges::find(*alsoPressed, extra) == alsoPressed->end()) {
          alsoPressed->push_back(extra);
        }
      }
    }
  }
  return matches;
}

std::optional<Action> ChordTracker::OnButton(const Button& button, bool pressed, TimeUtil::TimePoint now) {
  auto& device = DeviceFor(button);
  if (pressed) {
    if (IsHeld(device, button.mCode)) {
      return std::nullopt;// auto-repeat: nothing new
    }
    // Everything held is a switch, a detent or a key held for the game: this
    // press starts a new combination
    if (std::ranges::all_of(device.mHeld, [&](const HeldButton& held) { return IsLatched(held, now); })) {
      StartChord(device, now);
    }
    const HeldButton held {button.mCode, now};
    device.mHeld.insert(std::ranges::lower_bound(device.mHeld, held), held);
    Insert(device.mChord, button.mCode);
    return std::nullopt;
  }
  if (!IsHeld(device, button.mCode)) {
    return std::nullopt;// pressed before we were watching
  }
  const BoundAction* fired = nullptr;
  std::optional<Miss> miss;
  if (!Contains(device.mCarried, button.mCode)) {
    for (const auto& bound: mBindings) {
      std::vector<Button> alsoPressed;
      if (Matches(bound.mBinding, button, &alsoPressed)) {
        if (!fired || bound.mBinding.mButtons.size() > fired->mBinding.mButtons.size()) {
          fired = &bound;
        }
      } else if (!alsoPressed.empty() && !miss) {
        miss = Miss {bound.mBinding, std::move(alsoPressed)};
      }
    }
  }
  std::erase_if(device.mHeld, [&](const HeldButton& held) { return held.mCode == button.mCode; });
  Erase(device.mCarried, button.mCode);
  std::optional<Action> action;
  if (fired) {
    action = fired->mAction;
    // What's still held on the binding's devices starts a new chord, and
    // can't fire on its own release
    for (const auto& b: fired->mBinding.mButtons) {
      if (auto* d = FindDevice(b.mKind, b.mDevice)) {
        StartChord(*d, now);
        d->mCarried.clear();
        for (const auto& held: d->mHeld) {
          d->mCarried.push_back(held.mCode);
        }
      }
    }
  } else if (miss) {
    mMiss = std::move(miss);
  }
  if (device.mHeld.empty()) {
    device.mChord.clear();
    device.mCarried.clear();
  }
  return action;
}

std::optional<ChordTracker::Miss> ChordTracker::TakeMiss() {
  return std::exchange(mMiss, std::nullopt);
}

void ChordTracker::Resync(DeviceKind kind, const std::string& deviceID, const std::vector<uint32_t>& held) {
  auto* device = FindDevice(kind, deviceID);
  if (!device) {
    if (held.empty()) {
      return;
    }
    device = &mDevices.emplace_back(Device {kind, deviceID});
  }
  // Held since it was found: latched (switches, detents), and pressed before
  // it was lost: unknown chords, so they can't fire on their release
  device->mHeld.clear();
  for (const auto code: held) {
    device->mHeld.push_back(HeldButton {code, TimeUtil::TimePoint {}});
  }
  std::ranges::sort(device->mHeld);
  device->mCarried.clear();
  for (const auto& h: device->mHeld) {
    device->mCarried.push_back(h.mCode);
  }
  StartChord(*device, TimeUtil::Now());
}

void ChordTracker::Clear() {
  mDevices.clear();
  mMiss.reset();
}

void ChordTracker::DropKeys(const std::function<bool(uint32_t code)>& isUp) {
  if (auto* keyboard = FindDevice(DeviceKind::Keyboard, {})) {
    std::vector<uint32_t> dropped;
    for (const auto& held: keyboard->mHeld) {
      if (isUp(held.mCode)) {
        dropped.push_back(held.mCode);
      }
    }
    for (const auto code: dropped) {
      std::erase_if(keyboard->mHeld, [&](const HeldButton& held) { return held.mCode == code; });
      Erase(keyboard->mChord, code);
      Erase(keyboard->mCarried, code);
    }
    if (keyboard->mHeld.empty()) {
      keyboard->mChord.clear();
      keyboard->mCarried.clear();
    }
  }
}

std::vector<Button> ChordTracker::Held() const {
  std::vector<Button> held;
  for (const auto& device: mDevices) {
    for (const auto& h: device.mHeld) {
      held.push_back(Button {device.mKind, device.mDevice, h.mCode, device.mProduct, device.mName});
    }
  }
  return held;
}

}// namespace OpenDesktopVR::Keybinds
