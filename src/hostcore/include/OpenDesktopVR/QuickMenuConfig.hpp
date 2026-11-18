// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The quick menu's settings ("quickMenu" in settings.json; doc 09 "In-VR
// UI"; ADR-041) and its panel:
//
//   "quickMenu": {"button": true, "distanceCentimeters": 45,
//     "heightCentimeters": 25, "turnDegrees": -35, "tiltDegrees": 35}
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

namespace OpenDesktopVR::Host {

struct QuickMenuSettings {
  // The button while the menu is closed
  bool mButton {true};
  // Where it sits: this far from the eyes, this far below them, turned
  // round from straight ahead (positive to the left; the default is low
  // on the right, clear of the keyboard), tilted up to face you
  float mDistanceCentimeters {45};
  float mHeightCentimeters {25};
  float mTurnDegrees {-35};
  float mTiltDegrees {35};

  static constexpr float MinDistance = 30;
  static constexpr float MaxDistance = 80;
  static constexpr float MinHeight = -20;// above the eyes
  static constexpr float MaxHeight = 60;
  static constexpr float MaxTurn = 90;
  static constexpr float MinTilt = 0;
  static constexpr float MaxTilt = 80;

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
