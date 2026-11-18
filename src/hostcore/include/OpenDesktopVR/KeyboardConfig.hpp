// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The virtual keyboard's settings ("keyboard" in settings.json; doc 07,
// doc 09; ADR-036) and its panel:
//
//   "keyboard": {"enabled": true, "primaryLayout": "", "secondaryLayout": "",
//     "functionRow": true, "clickVolume": 0.5, "hideAfterEnter": false,
//     "keyMillimeters": 24, "typingOpacity": 1, "idleOpacity": 1,
//     "position": [0, -0.3, -0.4], "faceOrigin": false, "pitchDegrees": 45, ...}
//
// Where it sits and how it looks are a panel's settings (PanelPlacement,
// test round 19); its opacity is its own two (K9), not a panel's. Older
// files' "distanceCentimeters", "heightCentimeters" and "tiltDegrees" are
// read once and replaced.
//
// The keyboard is a special panel (K10): not in the "panels" list, so the
// settings window's panel list never shows it, and given to the renderer
// first, so a full list never leaves it out (VK-5). Reading is tolerant:
// anything missing or out of range reads as its default or nearest limit.
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/PanelPlacement.hpp>
#include <OpenDesktopVR/QuickMenuConfig.hpp>

#include <string>
#include <vector>

namespace OpenDesktopVR::Host {

struct KeyboardSettings {
  // "Virtual keyboard", under the hand settings (K10)
  bool mEnabled {true};
  // Windows layout IDs ("00000407"); the primary empty: "My Windows
  // layouts", the user's own list (K8)
  std::string mPrimaryLayout;
  std::string mSecondaryLayout;
  bool mFunctionRow {true};
  float mClickVolume {0.5f};// 0 is off (K11)
  bool mHideAfterEnter {false};// K6
  // Where it sits and how it looks (Q7: 50 cm from the eyes, 30 cm below
  // them, tilted 45 degrees up to face you; its opacity is K9's below)
  PanelPlacement mPlacement {DefaultPlacement()};
  // Its size: keys this far apart (doc 07: about 24 mm)
  float mKeyMillimeters {24};
  // K9: while you type (a hand near, or typed lately) and otherwise
  float mTypingOpacity {1.0f};
  float mIdleOpacity {1.0f};

  static constexpr float MinKey = 15;
  static constexpr float MaxKey = 32;

  static PanelPlacement DefaultPlacement() noexcept {
    return PanelPlacementConfig::FromDistance(50, 30, 0, 45);
  }

  bool operator==(const KeyboardSettings&) const = default;
};

namespace KeyboardConfig {
KeyboardSettings Read(const JsonUtil::Json& settings) noexcept;
void Write(JsonUtil::Json& settings, const KeyboardSettings& keyboard);
// Out-of-range values to their nearest limit, and non-numbers to defaults
KeyboardSettings Clean(KeyboardSettings keyboard) noexcept;
}// namespace KeyboardConfig

// The keyboard panel's fixed ID ("keyboard" in ASCII)
inline constexpr uint64_t KeyboardPanelID = 0x6B6579626F617264ull;

// The keyboard's panel, placed from its settings (by default straight
// ahead, below the eyes, tilted up); capped at 30 updates a second (doc 08)
PanelDefinition KeyboardPanel(const KeyboardSettings& keyboard);
// Its width in VR for keys `keyMillimeters` apart, and back
float KeyboardWidthMeters(float keyMillimeters) noexcept;
float KeyMillimetersFor(float widthMeters) noexcept;

// What the renderer gets: the keyboard first (VK-5) if `keyboard` is
// given, then the quick menu if `quickMenu` is (ADR-041), then the enabled
// panels
std::vector<PanelDefinition> PanelsForRenderer(
  std::vector<PanelDefinition> enabled, const KeyboardSettings* keyboard, const QuickMenuSettings* quickMenu = nullptr);

}// namespace OpenDesktopVR::Host
