// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The quick menu's settings ("quickMenu" in settings.json; doc 09 "In-VR
// UI"; ADR-041) and its panel:
//
//   "quickMenu": {"button": true, "widthMeters": 0.256,
//     "position": [0.21, -0.25, -0.31], "faceOrigin": false, "yawDegrees": -35,
//     "pitchDegrees": 35, "opacity": 1, ...}
//
// Where it sits and how it looks are a panel's settings (PanelPlacement,
// test round 19), set at the desk only: moving the menu you're tapping
// would be awkward. Round 19's "distanceCentimeters", "heightCentimeters",
// "turnDegrees" and "tiltDegrees" are read once and replaced.
//
// Like the keyboard's (K10), its panel isn't in the "panels" list: the
// settings window's list never shows it and hiding all panels never hides
// it, so the menu can always show them again. Closed, it's a small button
// where the menu opens (if "button" is on); its keybind opens it too.
// Reading is tolerant: anything missing or out of range reads as its
// default or nearest limit.
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/PanelPlacement.hpp>

namespace OpenDesktopVR::Host {

struct QuickMenuSettings {
  // The button while the menu is closed
  bool mButton {true};
  // Where it sits and how it looks: by default 45 cm from the eyes, 25 cm
  // below them, 35 degrees round to the right (clear of the keyboard),
  // tilted up 35 degrees to face you
  PanelPlacement mPlacement {DefaultPlacement()};
  // The open menu's width; never narrower than its own pixels at 2.5 a
  // millimetre, so its targets stay at least 20 mm (DU-6)
  float mWidthMeters {MinWidth};

  static constexpr float MinWidth = 0.256f;
  static constexpr float MaxWidth = 0.6f;

  static PanelPlacement DefaultPlacement() noexcept {
    return PanelPlacementConfig::FromDistance(45, 25, -35, 35);
  }

  bool operator==(const QuickMenuSettings&) const = default;
};

namespace QuickMenuConfig {
QuickMenuSettings Read(const JsonUtil::Json& settings) noexcept;
void Write(JsonUtil::Json& settings, const QuickMenuSettings& menu);
QuickMenuSettings Clean(QuickMenuSettings menu) noexcept;
}// namespace QuickMenuConfig

// The quick menu panel's fixed ID ("quickmnu" in ASCII)
inline constexpr uint64_t QuickMenuPanelID = 0x717569636B6D6E75ull;

// Its panel, placed from its settings, sized by its content (ADR-041),
// tapped and scrolled; capped at 30 updates a second (hover and presses)
PanelDefinition QuickMenuPanel(const QuickMenuSettings& menu);

}// namespace OpenDesktopVR::Host
