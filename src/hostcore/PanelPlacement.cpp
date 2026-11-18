// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/PanelConfig.hpp>
#include <OpenDesktopVR/PanelPlacement.hpp>
#include <OpenDesktopVR/QuickMenu.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OpenDesktopVR::Host::PanelPlacementConfig {

using JsonUtil::Json;

namespace {

float CleanFloat(float value, float fallback, float low, float high) noexcept {
  return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

}// namespace

PanelPlacement Clean(PanelPlacement p, const PanelPlacement& fallback) noexcept {
  using namespace QuickMenu;
  p.mPosition[0] = CleanFloat(p.mPosition[0], fallback.mPosition[0], -MaxAcrossMeters, MaxAcrossMeters);
  p.mPosition[1] = CleanFloat(p.mPosition[1], fallback.mPosition[1], -MaxUpDownMeters, MaxUpDownMeters);
  p.mPosition[2] = CleanFloat(p.mPosition[2], fallback.mPosition[2], -MaxAwayMeters, -MinAwayMeters);
  p.mYawDegrees = CleanFloat(p.mYawDegrees, fallback.mYawDegrees, -MaxTurnDegrees, MaxTurnDegrees);
  p.mPitchDegrees = CleanFloat(p.mPitchDegrees, fallback.mPitchDegrees, -MaxTiltDegrees, MaxTiltDegrees);
  const auto level = [](float value, float fallback) {
    return CleanFloat(value, fallback, PanelDefinition::MinimumLevel, 1.0f);
  };
  p.mOpacity = level(p.mOpacity, fallback.mOpacity);
  p.mFocusedOpacity = level(p.mFocusedOpacity, fallback.mFocusedOpacity);
  p.mBrightness = level(p.mBrightness, fallback.mBrightness);
  p.mTintColor &= 0xFFFFFF;
  return p;
}

std::optional<PanelPlacement> Read(const Json& section, const PanelPlacement& defaults) noexcept {
  try {
    const auto position = JsonUtil::Get<Json>(section, "position");
    if (!position) {
      return std::nullopt;
    }
    PanelPlacement p = defaults;
    if (position->is_array() && position->size() == 3) {
      for (std::size_t i = 0; i < 3; ++i) {
        if ((*position)[i].is_number()) {
          p.mPosition[i] = (*position)[i].get<float>();
        }
      }
    }
    p.mFaceOrigin = JsonUtil::GetOr<bool>(section, "faceOrigin", defaults.mFaceOrigin);
    p.mYawDegrees = JsonUtil::GetOr<float>(section, "yawDegrees", defaults.mYawDegrees);
    p.mPitchDegrees = JsonUtil::GetOr<float>(section, "pitchDegrees", defaults.mPitchDegrees);
    p.mOpacity = JsonUtil::GetOr<float>(section, "opacity", defaults.mOpacity);
    p.mFocusedOpacity = JsonUtil::GetOr<float>(section, "focusedOpacity", defaults.mFocusedOpacity);
    p.mBrightness = JsonUtil::GetOr<float>(section, "brightness", defaults.mBrightness);
    p.mTintEnabled = JsonUtil::GetOr<bool>(section, "tintEnabled", defaults.mTintEnabled);
    p.mTintColor = PanelConfig::ParseColor(JsonUtil::GetOr<std::string>(section, "tintColor", "")).value_or(defaults.mTintColor);
    return Clean(p, defaults);
  } catch (...) {
    return defaults;
  }
}

void Write(Json& section, const PanelPlacement& p) {
  section["position"] = Json::array({p.mPosition[0], p.mPosition[1], p.mPosition[2]});
  section["faceOrigin"] = p.mFaceOrigin;
  section["yawDegrees"] = p.mYawDegrees;
  section["pitchDegrees"] = p.mPitchDegrees;
  section["opacity"] = p.mOpacity;
  section["focusedOpacity"] = p.mFocusedOpacity;
  section["brightness"] = p.mBrightness;
  section["tintEnabled"] = p.mTintEnabled;
  section["tintColor"] = PanelConfig::FormatColor(p.mTintColor);
}

void ApplyTo(const PanelPlacement& p, PanelDefinition& panel) noexcept {
  std::copy(p.mPosition.begin(), p.mPosition.end(), panel.mPosition);
  panel.mFaceOrigin = p.mFaceOrigin;
  panel.mYawDegrees = p.mYawDegrees;
  panel.mPitchDegrees = p.mPitchDegrees;
  panel.mOpacity = p.mOpacity;
  panel.mFocusedOpacity = p.mFocusedOpacity;
  panel.mBrightness = p.mBrightness;
  panel.mTintEnabled = p.mTintEnabled;
  panel.mTintColor = p.mTintColor;
}

PanelPlacement From(const PanelDefinition& panel) noexcept {
  PanelPlacement p;
  std::copy(std::begin(panel.mPosition), std::end(panel.mPosition), p.mPosition.begin());
  p.mFaceOrigin = panel.mFaceOrigin;
  p.mYawDegrees = panel.mYawDegrees;
  p.mPitchDegrees = panel.mPitchDegrees;
  p.mOpacity = panel.mOpacity;
  p.mFocusedOpacity = panel.mFocusedOpacity;
  p.mBrightness = panel.mBrightness;
  p.mTintEnabled = panel.mTintEnabled;
  p.mTintColor = panel.mTintColor;
  return p;
}

void NudgeBy(PanelPlacement& p, Nudge what, float amount) noexcept {
  using namespace QuickMenu;
  if (!std::isfinite(amount)) {
    return;
  }
  const auto snap = [](float value, float grid) {
    const float snapped = std::round(value / grid) * grid;
    return snapped == 0.0f ? 0.0f : snapped;// never -0
  };
  constexpr float Tenth = 0.0001f;// metres
  constexpr float Hundredth = 0.01f;// degrees
  switch (what) {
    case Nudge::Across:
      p.mPosition[0] = snap(std::clamp(p.mPosition[0] + amount, -MaxAcrossMeters, MaxAcrossMeters), Tenth);
      return;
    case Nudge::Up:
      p.mPosition[1] = snap(std::clamp(p.mPosition[1] + amount, -MaxUpDownMeters, MaxUpDownMeters), Tenth);
      return;
    case Nudge::Away:
      p.mPosition[2] = -snap(std::clamp(-p.mPosition[2] + amount, MinAwayMeters, MaxAwayMeters), Tenth);
      return;
    case Nudge::Turn:
    case Nudge::Tilt:
      if (p.mFaceOrigin) {
        const auto facing = FacingOrigin(p.mPosition.data());
        p.mYawDegrees = facing.mYawDegrees;
        p.mPitchDegrees = facing.mPitchDegrees;
        p.mFaceOrigin = false;
      }
      if (what == Nudge::Turn) {
        float yaw = p.mYawDegrees + amount;
        yaw -= 360.0f * std::round(yaw / 360.0f);// wraps round, as the field does
        p.mYawDegrees = snap(std::clamp(yaw, -MaxTurnDegrees, MaxTurnDegrees), Hundredth);
      } else {
        p.mPitchDegrees = snap(std::clamp(p.mPitchDegrees + amount, -MaxTiltDegrees, MaxTiltDegrees), Hundredth);
      }
      return;
  }
}

PanelPlacement FromDistance(float distanceCm, float belowCm, float turnDegrees, float tiltDegrees) noexcept {
  PanelPlacement p;
  const float distance = std::max(distanceCm, 1.0f) / 100.0f;
  const float below = std::clamp(belowCm / 100.0f, -distance * 0.95f, distance * 0.95f);
  const float across = std::sqrt(std::max((distance * distance) - (below * below), 0.0001f));
  const float turn = turnDegrees * std::numbers::pi_v<float> / 180.0f;
  // + 0: straight ahead is 0, not -0 (which a field shows as "-0.0 cm")
  p.mPosition = {(-across * std::sin(turn)) + 0.0f, -below + 0.0f, -across * std::cos(turn)};
  p.mFaceOrigin = false;
  p.mYawDegrees = turnDegrees;
  p.mPitchDegrees = tiltDegrees;// its top tilted away: it faces up
  return p;
}

}// namespace OpenDesktopVR::Host::PanelPlacementConfig
