// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

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

bool ChordTracker::Matches(const Binding& binding, const Button& released) {
  if (std::ranges::find(binding.mButtons, released) == binding.mButtons.end()) {
    return false;
  }
  // Its other buttons are still held
  for (const auto& button: binding.mButtons) {
    const auto* device = FindDevice(button.mKind, button.mDevice);
    if (!device || !Contains(device->mHeld, button.mCode)) {
      return false;
    }
  }
  // And what was pressed on each of its devices was pressed for it
  for (const auto& button: binding.mButtons) {
    const auto* device = FindDevice(button.mKind, button.mDevice);
    std::vector<uint32_t> own;
    for (const auto& b: binding.mButtons) {
      if (b.mKind == button.mKind && b.mDevice == button.mDevice) {
        Insert(own, b.mCode);
      }
    }
    if (device->mKind == DeviceKind::Keyboard) {
      for (const auto code: device->mChord) {
        if (IsModifierCode(DeviceKind::Keyboard, code) && !Contains(own, code)) {
          return false;// another modifier: another combination
        }
      }
      if (!std::ranges::includes(device->mChord, own)) {
        return false;
      }
    } else if (device->mChord != own) {
      return false;
    }
  }
  return true;
}

std::optional<Action> ChordTracker::OnButton(const Button& button, bool pressed) {
  auto& device = DeviceFor(button);
  if (pressed) {
    if (!Contains(device.mHeld, button.mCode)) {// auto-repeat: nothing new
      Insert(device.mHeld, button.mCode);
      Insert(device.mChord, button.mCode);
    }
    return std::nullopt;
  }
  if (!Contains(device.mHeld, button.mCode)) {
    return std::nullopt;// pressed before we were watching
  }
  const BoundAction* fired = nullptr;
  if (!Contains(device.mCarried, button.mCode)) {
    for (const auto& bound: mBindings) {
      if ((!fired || bound.mBinding.mButtons.size() > fired->mBinding.mButtons.size()) && Matches(bound.mBinding, button)) {
        fired = &bound;
      }
    }
  }
  Erase(device.mHeld, button.mCode);
  Erase(device.mCarried, button.mCode);
  std::optional<Action> action;
  if (fired) {
    action = fired->mAction;
    // What's still held on the binding's devices starts a new chord, and
    // can't fire on its own release
    for (const auto& b: fired->mBinding.mButtons) {
      if (auto* d = FindDevice(b.mKind, b.mDevice)) {
        d->mChord = d->mHeld;
        d->mCarried = d->mHeld;
      }
    }
  }
  if (device.mHeld.empty()) {
    device.mChord.clear();
    device.mCarried.clear();
  }
  return action;
}

void ChordTracker::Resync(DeviceKind kind, const std::string& deviceID, const std::vector<uint32_t>& held) {
  auto* device = FindDevice(kind, deviceID);
  if (!device) {
    if (held.empty()) {
      return;
    }
    device = &mDevices.emplace_back(Device {kind, deviceID});
  }
  device->mHeld = held;
  std::ranges::sort(device->mHeld);
  device->mChord = device->mHeld;
  device->mCarried = device->mHeld;// pressed before it was lost: unknown chords
}

void ChordTracker::Clear() {
  mDevices.clear();
}

void ChordTracker::DropKeys(const std::function<bool(uint32_t code)>& isUp) {
  if (auto* keyboard = FindDevice(DeviceKind::Keyboard, {})) {
    std::vector<uint32_t> dropped;
    std::ranges::copy_if(keyboard->mHeld, std::back_inserter(dropped), isUp);
    for (const auto code: dropped) {
      Erase(keyboard->mHeld, code);
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
    for (const auto code: device.mHeld) {
      held.push_back(Button {device.mKind, device.mDevice, code, device.mProduct, device.mName});
    }
  }
  return held;
}

}// namespace OpenDesktopVR::Keybinds
