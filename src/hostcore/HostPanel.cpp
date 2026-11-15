// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/HostPanel.hpp>

#include <cmath>
#include <numbers>

namespace OpenDesktopVR::Host {

std::vector<PanelDefinition> DefaultTestPanels() {
  PanelDefinition animated;
  animated.mID = 0x0D0E'0001;
  animated.mName = L"Animated test pattern";
  animated.mPosition[0] = -0.22f;
  animated.mPosition[1] = -0.12f;
  animated.mPosition[2] = -0.55f;

  PanelDefinition still = animated;
  still.mID = 0x0D0E'0002;
  still.mName = L"Static test pattern";
  still.mPosition[0] = 0.22f;
  return {animated, still};
}

void PanelOrientation(const PanelDefinition& definition, float out[4]) noexcept {
  // Yaw 0 faces +Z (toward a viewer at the origin looking down -Z)
  float yaw = definition.mYawDegrees * std::numbers::pi_v<float> / 180.0f;
  if (definition.mFaceOrigin) {
    yaw = std::atan2(-definition.mPosition[0], -definition.mPosition[2]);
  }
  out[0] = 0;
  out[1] = std::sin(yaw / 2);
  out[2] = 0;
  out[3] = std::cos(yaw / 2);
}

}// namespace OpenDesktopVR::Host
