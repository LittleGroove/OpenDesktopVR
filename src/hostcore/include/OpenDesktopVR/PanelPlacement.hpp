// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Where a built-in panel (the keyboard, the quick menu) sits and how it
// looks (test round 19): the same settings as a panel's, under the same
// JSON keys and within the same limits (PanelConfig), so the settings
// window and the quick menu move and style them with the same controls.
//
//   "position": [x, y, z], "faceOrigin": false, "yawDegrees": 0,
//   "pitchDegrees": 45, "opacity": 1, "focusedOpacity": 1, "brightness": 1,
//   "tintEnabled": false, "tintColor": "#FF5A40"
//
// Untrusted (hand-edited files): anything odd reads as the default or the
// nearest limit.
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>

#include <array>
#include <optional>

namespace OpenDesktopVR::Host {

struct PanelPlacement {
  // From where the head was at the last recenter (LOCAL), as a panel's
  std::array<float, 3> mPosition {0, 0, -0.6f};
  bool mFaceOrigin {false};
  float mYawDegrees {0};
  float mPitchDegrees {0};
  // How it looks (PN-4, PN-5)
  float mOpacity {1.0f};
  float mFocusedOpacity {1.0f};
  float mBrightness {1.0f};
  bool mTintEnabled {false};
  uint32_t mTintColor {PanelDefinition::DefaultTintColor};

  bool operator==(const PanelPlacement&) const = default;
};

namespace PanelPlacementConfig {

// Within the settings window's limits; non-numbers take `fallback`'s value
PanelPlacement Clean(PanelPlacement placement, const PanelPlacement& fallback) noexcept;
// From a settings section; nullopt if it has no "position" (written by a
// version before test round 19)
std::optional<PanelPlacement> Read(const JsonUtil::Json& section, const PanelPlacement& defaults) noexcept;
void Write(JsonUtil::Json& section, const PanelPlacement& placement);

// Into and out of a panel's definition (the settings window's controls
// work on definitions)
void ApplyTo(const PanelPlacement& placement, PanelDefinition& panel) noexcept;
PanelPlacement From(const PanelDefinition& panel) noexcept;

// From the older settings: this far from the eyes and this far below them
// (centimetres), turned round from straight ahead (positive to the left)
// and tilted back
PanelPlacement FromDistance(float distanceCm, float belowCm, float turnDegrees, float tiltDegrees) noexcept;

}// namespace PanelPlacementConfig

}// namespace OpenDesktopVR::Host
