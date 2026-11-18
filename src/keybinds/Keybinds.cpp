// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Keybinds.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <algorithm>
#include <format>

namespace OpenDesktopVR::Keybinds {

using JsonUtil::Json;

namespace {

constexpr const char* HatDirectionNames[] = {"up", "right", "down", "left"};

std::string KeyName(uint32_t code) {
  const UINT vk = code & 0xFF;
  const bool extended = (code & ExtendedKey) != 0;
  switch (vk) {
    case VK_CONTROL:
      return "Ctrl";
    case VK_SHIFT:
      return "Shift";
    case VK_MENU:
      return "Alt";
    case VK_LWIN:
      return "Windows";
    default:
      break;
  }
  if (vk >= VK_F13 && vk <= VK_F24) {
    return std::format("F{}", vk - VK_F1 + 1);// often missing from the keyboard's names
  }
  const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
  if (scan != 0) {
    const LONG lParam = static_cast<LONG>(((scan & 0xFF) << 16) | ((extended || (scan & 0xE000)) ? (1 << 24) : 0));
    wchar_t name[64] {};
    if (const int length = GetKeyNameTextW(lParam, name, static_cast<int>(std::size(name))); length > 0) {
      return StringUtil::Narrow(std::wstring_view {name, static_cast<std::size_t>(length)});
    }
  }
  return std::format("Key {:#04x}", vk);
}

Json ButtonToJson(const Button& button) {
  if (button.mKind == DeviceKind::Keyboard) {
    return {
      {"key", ButtonName(button)},
      {"vk", button.mCode & 0xFF},
      {"extended", (button.mCode & ExtendedKey) != 0},
    };
  }
  Json json {
    {"device", button.mDevice},
    {"product", button.mProduct},
    {"deviceName", button.mDeviceName},
  };
  if (button.mCode >= HatBase) {
    json["hat"] = (button.mCode - HatBase) / 4;
    json["direction"] = HatDirectionNames[(button.mCode - HatBase) % 4];
  } else {
    json["button"] = button.mCode;
  }
  return json;
}

std::optional<Button> ButtonFromJson(const Json& json) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  Button button;
  if (const auto vk = JsonUtil::Get<uint32_t>(json, "vk")) {
    if (*vk == 0 || *vk > 0xFE) {
      return std::nullopt;
    }
    button.mKind = DeviceKind::Keyboard;
    button.mCode = *vk | (JsonUtil::GetOr<bool>(json, "extended", false) ? ExtendedKey : 0);
    return button;
  }
  button.mKind = DeviceKind::Controller;
  button.mDevice = JsonUtil::GetOr<std::string>(json, "device", "");
  button.mProduct = JsonUtil::GetOr<std::string>(json, "product", "");
  button.mDeviceName = JsonUtil::GetOr<std::string>(json, "deviceName", "");
  if (button.mDevice.empty() || button.mDevice.size() > 64) {
    return std::nullopt;
  }
  if (const auto index = JsonUtil::Get<uint32_t>(json, "button")) {
    if (*index >= 128) {
      return std::nullopt;
    }
    button.mCode = *index;
    return button;
  }
  const auto hat = JsonUtil::Get<uint32_t>(json, "hat");
  const auto direction = JsonUtil::GetOr<std::string>(json, "direction", "");
  const auto found = std::ranges::find(HatDirectionNames, direction);
  if (!hat || *hat >= 4 || found == std::end(HatDirectionNames)) {
    return std::nullopt;
  }
  button.mCode = HatCode(*hat, static_cast<HatDirection>(found - std::begin(HatDirectionNames)));
  return button;
}

}// namespace

bool IsModifier(const Button& button) noexcept {
  if (button.mKind != DeviceKind::Keyboard) {
    return false;
  }
  const auto vk = button.mCode & 0xFF;
  return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN;
}

bool Binding::IsValid() const noexcept {
  return !mButtons.empty() && mButtons.size() <= MaxButtons
    && !std::ranges::all_of(mButtons, [](const Button& b) { return IsModifier(b); });
}

Binding Binding::From(std::vector<Button> buttons) {
  std::ranges::sort(buttons);
  const auto [first, last] = std::ranges::unique(buttons);
  buttons.erase(first, last);
  return Binding {std::move(buttons)};
}

std::string ActionName(ActionKind kind) {
  switch (kind) {
    case ActionKind::ToggleAllPanels:
      return "Show or hide all panels";
    case ActionKind::TogglePanel:
      return "Show or hide";
    case ActionKind::BrightnessUp:
      return "Brighter";
    case ActionKind::BrightnessDown:
      return "Dimmer";
    case ActionKind::ToggleTint:
      return "Tint on or off";
    case ActionKind::ToggleKeyboard:
      return "Show or hide the keyboard";
    case ActionKind::Recenter:
      return "Recenter";
    case ActionKind::NextProfile:
      return "Next profile";
    case ActionKind::PreviousProfile:
      return "Previous profile";
    case ActionKind::ToggleQuickMenu:
      return "Open or close the quick menu";
  }
  return "?";
}

const char* ActionKey(ActionKind kind) noexcept {
  switch (kind) {
    case ActionKind::ToggleAllPanels:
      return "toggleAllPanels";
    case ActionKind::TogglePanel:
      return "toggle";
    case ActionKind::BrightnessUp:
      return "brightnessUp";
    case ActionKind::BrightnessDown:
      return "brightnessDown";
    case ActionKind::ToggleTint:
      return "toggleTint";
    case ActionKind::ToggleKeyboard:
      return "toggleKeyboard";
    case ActionKind::Recenter:
      return "recenter";
    case ActionKind::NextProfile:
      return "nextProfile";
    case ActionKind::PreviousProfile:
      return "previousProfile";
    case ActionKind::ToggleQuickMenu:
      return "toggleQuickMenu";
  }
  return "";
}

std::string ButtonName(const Button& button) {
  if (button.mKind == DeviceKind::Keyboard) {
    return KeyName(button.mCode);
  }
  const auto device = button.mDeviceName.empty() ? std::string {"Controller"} : button.mDeviceName;
  return std::format("{}: {}", device, ControllerCodeName(button.mCode));
}

std::string ControllerCodeName(uint32_t code) {
  if (code >= HatBase) {
    return std::format("hat {} {}", (code - HatBase) / 4 + 1, HatDirectionNames[(code - HatBase) % 4]);
  }
  return std::format("button {}", code + 1);
}

std::string BindingName(const Binding& binding) {
  // Keyboard modifiers first, as people write them
  std::vector<const Button*> ordered;
  for (const auto& button: binding.mButtons) {
    ordered.push_back(&button);
  }
  std::ranges::stable_partition(ordered, [](const Button* b) { return IsModifier(*b); });
  std::string name;
  for (const auto* button: ordered) {
    name += (name.empty() ? "" : " + ") + ButtonName(*button);
  }
  return name;
}

Json ToJson(const Binding& binding) {
  Json list = Json::array();
  for (const auto& button: binding.mButtons) {
    list.push_back(ButtonToJson(button));
  }
  return list;
}

Binding BindingFromJson(const Json& json) noexcept {
  try {
    if (!json.is_array() || json.size() > MaxButtons) {
      return {};
    }
    std::vector<Button> buttons;
    for (const auto& entry: json) {
      auto button = ButtonFromJson(entry);
      if (!button) {
        return {};// one bad button: the whole binding is dropped
      }
      buttons.push_back(std::move(*button));
    }
    auto binding = Binding::From(std::move(buttons));
    return binding.IsValid() ? binding : Binding {};
  } catch (...) {
    return {};
  }
}

Json ToJson(const Slots& slots) {
  return Json::array({ToJson(slots[0]), ToJson(slots[1])});
}

Slots SlotsFromJson(const Json& json) noexcept {
  Slots slots;
  try {
    if (json.is_array()) {
      for (std::size_t i = 0; i < slots.size() && i < json.size(); ++i) {
        slots[i] = BindingFromJson(json[i]);
      }
    }
  } catch (...) {
    return {};
  }
  return slots;
}

}// namespace OpenDesktopVR::Keybinds
