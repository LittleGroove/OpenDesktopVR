// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Keybinds (doc 09 "Keybinds", UI-2, UI-3): what a binding is, the actions
// it can trigger, how it's stored and how it's named.
//
// - A button is a keyboard key (its virtual-key code, plus the "extended"
//   bit that tells the arrow cluster from the number pad; left and right
//   Ctrl, Shift, Alt and Windows count as one) or a game controller's button or hat
//   direction (DirectInput; the device by its instance GUID, with its
//   product GUID and name so it can be found again and named while
//   unplugged).
// - A binding is one to four buttons, pressed together. A keyboard modifier
//   (Ctrl, Shift, Alt, Windows) on its own is never a binding.
// - Each action has two slots.
// - Stored as readable JSON (UI-5): {"key": "F13"} style names are written
//   alongside the codes, and only the codes are read back.
#pragma once

#include <OpenDesktopVR/JsonUtil.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::Keybinds {

enum class DeviceKind : uint8_t { Keyboard, Controller };

// Keyboard codes: the virtual-key code, | ExtendedKey for E0-prefixed keys
inline constexpr uint32_t ExtendedKey = 0x100;
// Controller codes: buttons are 0..127; hat directions start here
inline constexpr uint32_t HatBase = 0x1000;
enum class HatDirection : uint32_t { Up = 0, Right = 1, Down = 2, Left = 3 };
constexpr uint32_t HatCode(uint32_t hat, HatDirection direction) noexcept {
  return HatBase + hat * 4 + static_cast<uint32_t>(direction);
}

struct Button {
  DeviceKind mKind {DeviceKind::Keyboard};
  // Controllers: the instance GUID ("{...}"); empty for the keyboard
  std::string mDevice;
  uint32_t mCode {0};
  // Not part of the identity: for finding a controller again on another
  // port, and for naming it while it's unplugged
  std::string mProduct;
  std::string mDeviceName;

  bool operator==(const Button& other) const noexcept {
    return mKind == other.mKind && mDevice == other.mDevice && mCode == other.mCode;
  }
  auto operator<=>(const Button& other) const noexcept {
    if (auto c = mKind <=> other.mKind; c != 0) {
      return c;
    }
    if (auto c = mDevice <=> other.mDevice; c != 0) {
      return c;
    }
    return mCode <=> other.mCode;
  }
};

// True for Ctrl, Shift, Alt and Windows keys (either side)
bool IsModifier(const Button& button) noexcept;

inline constexpr std::size_t MaxButtons = 4;

struct Binding {
  std::vector<Button> mButtons;// sorted, no duplicates

  bool operator==(const Binding& other) const noexcept {
    return mButtons == other.mButtons;
  }
  bool IsEmpty() const noexcept {
    return mButtons.empty();
  }
  // 1..4 buttons, and not only keyboard modifiers
  bool IsValid() const noexcept;
  // Sorted and without duplicates
  static Binding From(std::vector<Button> buttons);
};

// Two slots per action (doc 09)
using Slots = std::array<Binding, 2>;

enum class ActionKind : uint8_t {
  ToggleAllPanels,// global: show or hide all (PN-6)
  TogglePanel,    // per panel: show or hide it
  BrightnessUp,   // per panel (PN-5)
  BrightnessDown,
  ToggleTint,
  ToggleKeyboard,// global: show or hide the keyboard (K5, ADR-036)
  Recenter,      // global: panels where the head is now (PN-7, ADR-037)
  NextProfile,   // global (ADR-038)
  PreviousProfile,
  ToggleQuickMenu,// global: open or close the quick menu in VR (ADR-041)
};
inline constexpr ActionKind LastActionKind = ActionKind::ToggleQuickMenu;
inline constexpr std::array PanelActions {
  ActionKind::TogglePanel, ActionKind::BrightnessUp, ActionKind::BrightnessDown, ActionKind::ToggleTint};
// Stored in "keybinds", not with a panel
inline constexpr std::array GlobalActions {ActionKind::ToggleAllPanels, ActionKind::ToggleKeyboard, ActionKind::Recenter,
  ActionKind::NextProfile, ActionKind::PreviousProfile, ActionKind::ToggleQuickMenu};
constexpr bool IsGlobal(ActionKind kind) noexcept {
  for (const auto global: GlobalActions) {
    if (global == kind) {
      return true;
    }
  }
  return false;
}

struct Action {
  ActionKind mKind {ActionKind::ToggleAllPanels};
  uint64_t mPanelID {0};// per-panel actions
  bool operator==(const Action&) const = default;
};

// For the settings window and logs: "Show or hide all panels"
std::string ActionName(ActionKind kind);
// "Ctrl + F13", "T.16000M: button 5", "T.16000M: hat 1 up"
std::string ButtonName(const Button& button);
// A controller code without its device: "button 5", "hat 1 up"
std::string ControllerCodeName(uint32_t code);
std::string BindingName(const Binding& binding);

// The stored form. Untrusted (TH-9): anything malformed reads as empty.
JsonUtil::Json ToJson(const Binding& binding);
Binding BindingFromJson(const JsonUtil::Json& json) noexcept;
JsonUtil::Json ToJson(const Slots& slots);
Slots SlotsFromJson(const JsonUtil::Json& json) noexcept;
// The JSON key of each action ("toggleAllPanels", "toggle", ...)
const char* ActionKey(ActionKind kind) noexcept;

}// namespace OpenDesktopVR::Keybinds
