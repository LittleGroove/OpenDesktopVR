// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/KeyboardGeometry.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace OpenDesktopVR::Host {

using JsonUtil::Json;

namespace KeyboardConfig {

namespace {

float CleanFloat(float value, float fallback, float low, float high) noexcept {
  return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

// A layout ID is eight hex digits; anything else reads as none
std::string CleanLayout(std::string klid) {
  if (klid.size() != 8 || !std::ranges::all_of(klid, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) {
    return {};
  }
  return klid;
}

}// namespace

KeyboardSettings Clean(KeyboardSettings keyboard) noexcept {
  const KeyboardSettings defaults;
  keyboard.mPrimaryLayout = CleanLayout(std::move(keyboard.mPrimaryLayout));
  keyboard.mSecondaryLayout = CleanLayout(std::move(keyboard.mSecondaryLayout));
  keyboard.mClickVolume = CleanFloat(keyboard.mClickVolume, defaults.mClickVolume, 0, 1);
  keyboard.mPlacement = PanelPlacementConfig::Clean(keyboard.mPlacement, defaults.mPlacement);
  keyboard.mKeyMillimeters
    = CleanFloat(keyboard.mKeyMillimeters, defaults.mKeyMillimeters, KeyboardSettings::MinKey, KeyboardSettings::MaxKey);
  keyboard.mTypingOpacity
    = CleanFloat(keyboard.mTypingOpacity, defaults.mTypingOpacity, PanelDefinition::MinimumLevel, 1);
  keyboard.mIdleOpacity = CleanFloat(keyboard.mIdleOpacity, defaults.mIdleOpacity, PanelDefinition::MinimumLevel, 1);
  return keyboard;
}

KeyboardSettings Read(const Json& settings) noexcept {
  KeyboardSettings keyboard;
  try {
    const auto section = JsonUtil::GetOr<Json>(settings, "keyboard", Json {});
    keyboard.mEnabled = JsonUtil::GetOr<bool>(section, "enabled", keyboard.mEnabled);
    keyboard.mPrimaryLayout = JsonUtil::GetOr<std::string>(section, "primaryLayout", {});
    keyboard.mSecondaryLayout = JsonUtil::GetOr<std::string>(section, "secondaryLayout", {});
    keyboard.mFunctionRow = JsonUtil::GetOr<bool>(section, "functionRow", keyboard.mFunctionRow);
    keyboard.mClickVolume = JsonUtil::GetOr<float>(section, "clickVolume", keyboard.mClickVolume);
    keyboard.mHideAfterEnter = JsonUtil::GetOr<bool>(section, "hideAfterEnter", keyboard.mHideAfterEnter);
    if (const auto placement = PanelPlacementConfig::Read(section, keyboard.mPlacement)) {
      keyboard.mPlacement = *placement;
    } else {
      // Before test round 19: straight ahead, this far away and below,
      // tilted (within the limits it had then)
      const auto old = [&](const char* key, float fallback, float low, float high) {
        const float value = JsonUtil::GetOr<float>(section, key, fallback);
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
      };
      keyboard.mPlacement = PanelPlacementConfig::FromDistance(old("distanceCentimeters", 50, 30, 80),
        old("heightCentimeters", 30, 0, 60), 0, old("tiltDegrees", 45, 0, 80));
    }
    keyboard.mKeyMillimeters = JsonUtil::GetOr<float>(section, "keyMillimeters", keyboard.mKeyMillimeters);
    keyboard.mTypingOpacity = JsonUtil::GetOr<float>(section, "typingOpacity", keyboard.mTypingOpacity);
    keyboard.mIdleOpacity = JsonUtil::GetOr<float>(section, "idleOpacity", keyboard.mIdleOpacity);
  } catch (...) {
    return {};
  }
  return Clean(std::move(keyboard));
}

void Write(Json& settings, const KeyboardSettings& keyboard) {
  Json section = JsonUtil::GetOr<Json>(settings, "keyboard", Json::object());
  if (!section.is_object()) {
    section = Json::object();
  }
  section["enabled"] = keyboard.mEnabled;
  section["primaryLayout"] = keyboard.mPrimaryLayout;
  section["secondaryLayout"] = keyboard.mSecondaryLayout;
  section["functionRow"] = keyboard.mFunctionRow;
  section["clickVolume"] = keyboard.mClickVolume;
  section["hideAfterEnter"] = keyboard.mHideAfterEnter;
  PanelPlacementConfig::Write(section, keyboard.mPlacement);
  for (const char* old: {"distanceCentimeters", "heightCentimeters", "tiltDegrees"}) {
    section.erase(old);// replaced by the placement (test round 19)
  }
  section["keyMillimeters"] = keyboard.mKeyMillimeters;
  section["typingOpacity"] = keyboard.mTypingOpacity;
  section["idleOpacity"] = keyboard.mIdleOpacity;
  settings["keyboard"] = std::move(section);
}

}// namespace KeyboardConfig

PanelDefinition KeyboardPanel(const KeyboardSettings& settings) {
  const auto keyboard = KeyboardConfig::Clean(settings);
  PanelDefinition panel;
  panel.mID = KeyboardPanelID;
  panel.mName = L"Keyboard";
  panel.mKind = PanelKind::Keyboard;
  panel.mAnimated = false;
  PanelPlacementConfig::ApplyTo(keyboard.mPlacement, panel);
  // Its opacity is K9's, drawn by the keyboard itself
  panel.mOpacity = 1.0f;
  panel.mFocusedOpacity = 1.0f;
  panel.mWidthMeters = KeyboardWidthMeters(keyboard.mKeyMillimeters);
  panel.mMaxUpdatesPerSecond = 30;// with its preview strip (doc 08)
  panel.mKeepUpdatingWhenCovered = false;
  // Typed on (ADR-036), never pointed at
  panel.mTouch = false;
  panel.mScroll = false;
  return panel;
}

namespace {
float WidthUnits() noexcept {
  static const float units = VirtualKeyboard::BuildGeometry({}).mWidth;
  return units;
}
}// namespace

float KeyboardWidthMeters(float keyMillimeters) noexcept {
  return WidthUnits() * keyMillimeters / 1000.0f;
}

float KeyMillimetersFor(float widthMeters) noexcept {
  return widthMeters * 1000.0f / WidthUnits();
}

std::vector<PanelDefinition> PanelsForRenderer(
  std::vector<PanelDefinition> enabled, const KeyboardSettings* keyboard, const QuickMenuSettings* quickMenu) {
  if (!keyboard && !quickMenu) {
    return enabled;
  }
  std::vector<PanelDefinition> panels;
  panels.reserve(enabled.size() + 2);
  if (keyboard) {
    panels.push_back(KeyboardPanel(*keyboard));
  }
  if (quickMenu) {
    panels.push_back(QuickMenuPanel(*quickMenu));
  }
  for (auto& panel: enabled) {
    if (panel.mID != KeyboardPanelID && panel.mID != QuickMenuPanelID) {
      panels.push_back(std::move(panel));
    }
  }
  return panels;
}

}// namespace OpenDesktopVR::Host
