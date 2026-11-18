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
  keyboard.mDistanceCentimeters = CleanFloat(
    keyboard.mDistanceCentimeters, defaults.mDistanceCentimeters, KeyboardSettings::MinDistance, KeyboardSettings::MaxDistance);
  keyboard.mHeightCentimeters = CleanFloat(
    keyboard.mHeightCentimeters, defaults.mHeightCentimeters, KeyboardSettings::MinHeight, KeyboardSettings::MaxHeight);
  keyboard.mTiltDegrees
    = CleanFloat(keyboard.mTiltDegrees, defaults.mTiltDegrees, KeyboardSettings::MinTilt, KeyboardSettings::MaxTilt);
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
    keyboard.mDistanceCentimeters = JsonUtil::GetOr<float>(section, "distanceCentimeters", keyboard.mDistanceCentimeters);
    keyboard.mHeightCentimeters = JsonUtil::GetOr<float>(section, "heightCentimeters", keyboard.mHeightCentimeters);
    keyboard.mTiltDegrees = JsonUtil::GetOr<float>(section, "tiltDegrees", keyboard.mTiltDegrees);
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
  section["distanceCentimeters"] = keyboard.mDistanceCentimeters;
  section["heightCentimeters"] = keyboard.mHeightCentimeters;
  section["tiltDegrees"] = keyboard.mTiltDegrees;
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
  // Straight ahead and below the eyes, `distance` from them
  const float distance = keyboard.mDistanceCentimeters / 100.0f;
  const float height = std::min(keyboard.mHeightCentimeters / 100.0f, distance * 0.95f);
  panel.mPosition[0] = 0;
  panel.mPosition[1] = -height;
  panel.mPosition[2] = -std::sqrt(std::max((distance * distance) - (height * height), 0.0001f));
  panel.mFaceOrigin = false;
  panel.mYawDegrees = 0;
  panel.mPitchDegrees = keyboard.mTiltDegrees;// its top tilted away: it faces up
  static const float widthUnits = VirtualKeyboard::BuildGeometry({}).mWidth;
  panel.mWidthMeters = widthUnits * keyboard.mKeyMillimeters / 1000.0f;
  panel.mMaxUpdatesPerSecond = 30;// with its preview strip (doc 08)
  panel.mKeepUpdatingWhenCovered = false;
  // Typed on (ADR-036), never pointed at
  panel.mTouch = false;
  panel.mScroll = false;
  return panel;
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
