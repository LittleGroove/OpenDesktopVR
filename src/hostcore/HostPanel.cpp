// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/HostPanel.hpp>

#include <cmath>
#include <numbers>
#include <random>

namespace OpenDesktopVR::Host {

std::vector<PanelDefinition> DefaultTestPanels() {
  PanelDefinition animated;
  animated.mID = 0x0D0E'0001;
  animated.mName = L"Animated test pattern";
  animated.mKind = PanelKind::TestPattern;
  animated.mAnimated = true;
  animated.mPosition[0] = -0.22f;
  animated.mPosition[1] = -0.12f;
  animated.mPosition[2] = -0.55f;

  PanelDefinition still = animated;
  still.mID = 0x0D0E'0002;
  still.mName = L"Static test pattern";
  still.mAnimated = false;
  still.mPosition[0] = 0.22f;
  return {animated, still};
}

PanelDefinition NewWindowPanel(const WindowUtil::WindowInfo& window, std::size_t existingWindowPanels) {
  PanelDefinition panel;
  panel.mID = NewPanelID();
  panel.mName = window.mTitle.empty() ? WindowUtil::ExecutableName(window) : window.mTitle;
  panel.mKind = PanelKind::Window;
  panel.mWindow = WindowMatch::FromWindow(window);
  // A grid in front of the viewer: three across, then a row lower
  const auto column = static_cast<float>(existingWindowPanels % 3);
  const auto row = static_cast<float>((existingWindowPanels / 3) % 3);
  panel.mPosition[0] = (column - 1.0f) * 0.5f;
  panel.mPosition[1] = 0.05f - row * 0.35f;
  panel.mPosition[2] = -0.7f;
  panel.mWidthMeters = 0.45f;
  return panel;
}

uint64_t NewPanelID() noexcept {
  static std::random_device device;
  std::mt19937_64 generator {(static_cast<uint64_t>(device()) << 32) ^ device()};
  uint64_t id = 0;
  while (id == 0) {
    id = generator();
  }
  return id;
}

namespace {
constexpr float DegreesPerRadian = 180.0f / std::numbers::pi_v<float>;
}

Facing FacingOrigin(const float position[3]) noexcept {
  // Toward the origin: turned by the horizontal direction, tilted by the
  // vertical one (a panel below the eyes faces up)
  const float horizontal = std::sqrt(position[0] * position[0] + position[2] * position[2]);
  return {
    std::atan2(-position[0], -position[2]) * DegreesPerRadian,
    std::atan2(-position[1], horizontal) * DegreesPerRadian,
  };
}

void PanelOrientation(const PanelDefinition& definition, float out[4]) noexcept {
  // Yaw 0, pitch 0 faces +Z (toward a viewer at the origin looking down -Z)
  const auto facing = definition.mFaceOrigin ? FacingOrigin(definition.mPosition)
                                             : Facing {definition.mYawDegrees, definition.mPitchDegrees};
  const float yaw = facing.mYawDegrees / DegreesPerRadian;
  // A rotation of -pitch about +X turns the +Z normal up for a positive pitch
  const float pitch = -facing.mPitchDegrees / DegreesPerRadian;
  // Turn about Y, then tilt about the panel's own X: q = qYaw * qPitch
  const float sy = std::sin(yaw / 2);
  const float cy = std::cos(yaw / 2);
  const float sx = std::sin(pitch / 2);
  const float cx = std::cos(pitch / 2);
  out[0] = cy * sx;
  out[1] = sy * cx;
  out[2] = -sy * sx;
  out[3] = cy * cx;
}

}// namespace OpenDesktopVR::Host
