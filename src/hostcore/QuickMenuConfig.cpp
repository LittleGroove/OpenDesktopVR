// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

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
  menu.mPlacement = PanelPlacementConfig::Clean(menu.mPlacement, defaults.mPlacement);
  menu.mWidthMeters = CleanFloat(menu.mWidthMeters, defaults.mWidthMeters, QuickMenuSettings::MinWidth, QuickMenuSettings::MaxWidth);
  return menu;
}

QuickMenuSettings Read(const Json& settings) noexcept {
  QuickMenuSettings menu;
  try {
    const auto section = JsonUtil::GetOr<Json>(settings, "quickMenu", Json {});
    menu.mButton = JsonUtil::GetOr<bool>(section, "button", menu.mButton);
    menu.mWidthMeters = JsonUtil::GetOr<float>(section, "widthMeters", menu.mWidthMeters);
    if (const auto placement = PanelPlacementConfig::Read(section, menu.mPlacement)) {
      menu.mPlacement = *placement;
    } else if (section.is_object() && section.contains("distanceCentimeters")) {
      // Test round 19's (within the limits it had then)
      const auto old = [&](const char* key, float fallback, float low, float high) {
        const float value = JsonUtil::GetOr<float>(section, key, fallback);
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
      };
      menu.mPlacement = PanelPlacementConfig::FromDistance(old("distanceCentimeters", 45, 30, 80),
        old("heightCentimeters", 25, -20, 60), old("turnDegrees", -35, -90, 90), old("tiltDegrees", 35, 0, 80));
    }
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
  section["widthMeters"] = menu.mWidthMeters;
  PanelPlacementConfig::Write(section, menu.mPlacement);
  for (const char* old: {"distanceCentimeters", "heightCentimeters", "turnDegrees", "tiltDegrees"}) {
    section.erase(old);// replaced by the placement (test round 19)
  }
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
  PanelPlacementConfig::ApplyTo(menu.mPlacement, panel);
  // Its size follows its pixels (the button and the open menu), scaled up
  // from 2.5 a millimetre by a wider setting, never down (DU-6)
  panel.mWidthMeters = menu.mWidthMeters;
  panel.mMetersPerPixel = menu.mWidthMeters / static_cast<float>(QuickMenu::Width);
  panel.mMaxUpdatesPerSecond = 30;
  panel.mKeepUpdatingWhenCovered = false;
  panel.mTouch = true;
  panel.mScroll = true;// long lists
  return panel;
}

}// namespace OpenDesktopVR::Host
