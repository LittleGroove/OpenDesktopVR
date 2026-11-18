// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The virtual keyboard's settings ("keyboard" in settings.json; doc 07,
// doc 09; ADR-036) and its panel:
//
//   "keyboard": {"enabled": true, "primaryLayout": "", "secondaryLayout": "",
//     "functionRow": true, "clickVolume": 0.5, "hideAfterEnter": false,
//     "distanceCentimeters": 50, "heightCentimeters": 30, "tiltDegrees": 45,
//     "keyMillimeters": 24, "typingOpacity": 1, "idleOpacity": 1}
//
// The keyboard is a special panel (K10): not in the "panels" list, so the
// settings window's panel list never shows it, and given to the renderer
// first, so a full list never leaves it out (VK-5). Reading is tolerant:
// anything missing or out of range reads as its default or nearest limit.
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
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
  // Where it sits (Q7): this far from the eyes, this far below them, tilted
  // up to face you, with keys this big (doc 07: 45-55 cm, about 24 mm)
  float mDistanceCentimeters {50};
  float mHeightCentimeters {30};
  float mTiltDegrees {45};
  float mKeyMillimeters {24};
  // K9: while you type (a hand near, or typed lately) and otherwise
  float mTypingOpacity {1.0f};
  float mIdleOpacity {1.0f};

  static constexpr float MinDistance = 30;
  static constexpr float MaxDistance = 80;
  static constexpr float MinHeight = 0;
  static constexpr float MaxHeight = 60;
  static constexpr float MinTilt = 0;
  static constexpr float MaxTilt = 80;
  static constexpr float MinKey = 15;
  static constexpr float MaxKey = 32;

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

// The keyboard's panel, placed from its settings: straight ahead, below the
// eyes, tilted up; capped at 30 updates a second (doc 08)
PanelDefinition KeyboardPanel(const KeyboardSettings& keyboard);

// What the renderer gets: the keyboard first (VK-5) if `keyboard` is
// given, then the quick menu if `quickMenu` is (ADR-041), then the enabled
// panels
std::vector<PanelDefinition> PanelsForRenderer(
  std::vector<PanelDefinition> enabled, const KeyboardSettings* keyboard, const QuickMenuSettings* quickMenu = nullptr);

}// namespace OpenDesktopVR::Host
