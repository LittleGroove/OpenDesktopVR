// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/QuickMenu.hpp>
#include <OpenDesktopVR/QuickMenuConfig.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OpenDesktopVR::Host {

using JsonUtil::Json;

namespace QuickMenuConfig {

namespace {

float CleanFloat(float value, float fallback, float low, float high) noexcept {
  return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

}// namespace

QuickMenuSettings Clean(QuickMenuSettings menu) noexcept {
  const QuickMenuSettings defaults;
  using S = QuickMenuSettings;
  menu.mDistanceCentimeters = CleanFloat(menu.mDistanceCentimeters, defaults.mDistanceCentimeters, S::MinDistance, S::MaxDistance);
  menu.mHeightCentimeters = CleanFloat(menu.mHeightCentimeters, defaults.mHeightCentimeters, S::MinHeight, S::MaxHeight);
  menu.mTurnDegrees = CleanFloat(menu.mTurnDegrees, defaults.mTurnDegrees, -S::MaxTurn, S::MaxTurn);
  menu.mTiltDegrees = CleanFloat(menu.mTiltDegrees, defaults.mTiltDegrees, S::MinTilt, S::MaxTilt);
  return menu;
}

QuickMenuSettings Read(const Json& settings) noexcept {
  QuickMenuSettings menu;
  try {
    const auto section = JsonUtil::GetOr<Json>(settings, "quickMenu", Json {});
    menu.mButton = JsonUtil::GetOr<bool>(section, "button", menu.mButton);
    menu.mDistanceCentimeters = JsonUtil::GetOr<float>(section, "distanceCentimeters", menu.mDistanceCentimeters);
    menu.mHeightCentimeters = JsonUtil::GetOr<float>(section, "heightCentimeters", menu.mHeightCentimeters);
    menu.mTurnDegrees = JsonUtil::GetOr<float>(section, "turnDegrees", menu.mTurnDegrees);
    menu.mTiltDegrees = JsonUtil::GetOr<float>(section, "tiltDegrees", menu.mTiltDegrees);
  } catch (...) {
    return {};
  }
  return Clean(menu);
}

void Write(Json& settings, const QuickMenuSettings& menu) {
  Json section = JsonUtil::GetOr<Json>(settings, "quickMenu", Json::object());
  if (!section.is_object()) {
    section = Json::object();
  }
  section["button"] = menu.mButton;
  section["distanceCentimeters"] = menu.mDistanceCentimeters;
  section["heightCentimeters"] = menu.mHeightCentimeters;
  section["turnDegrees"] = menu.mTurnDegrees;
  section["tiltDegrees"] = menu.mTiltDegrees;
  settings["quickMenu"] = std::move(section);
}

}// namespace QuickMenuConfig

PanelDefinition QuickMenuPanel(const QuickMenuSettings& settings) {
  const auto menu = QuickMenuConfig::Clean(settings);
  PanelDefinition panel;
  panel.mID = QuickMenuPanelID;
  panel.mName = L"Quick menu";
  panel.mKind = PanelKind::QuickMenu;
  panel.mAnimated = false;
  // `distance` from the eyes, `height` below them, turned round from
  // straight ahead (positive to the left) and facing back toward the eyes
  const float distance = menu.mDistanceCentimeters / 100.0f;
  const float height = std::clamp(menu.mHeightCentimeters / 100.0f, -distance * 0.95f, distance * 0.95f);
  const float across = std::sqrt(std::max((distance * distance) - (height * height), 0.0001f));
  const float turn = menu.mTurnDegrees * std::numbers::pi_v<float> / 180.0f;
  panel.mPosition[0] = -across * std::sin(turn);
  panel.mPosition[1] = -height;
  panel.mPosition[2] = -across * std::cos(turn);
  panel.mFaceOrigin = false;
  panel.mYawDegrees = menu.mTurnDegrees;
  panel.mPitchDegrees = menu.mTiltDegrees;// its top tilted away: it faces up
  panel.mMetersPerPixel = QuickMenu::MetersPerPixel;
  panel.mWidthMeters = static_cast<float>(QuickMenu::Width) * QuickMenu::MetersPerPixel;
  panel.mMaxUpdatesPerSecond = 30;
  panel.mKeepUpdatingWhenCovered = false;
  panel.mTouch = true;
  panel.mScroll = true;// long lists
  return panel;
}

}// namespace OpenDesktopVR::Host
