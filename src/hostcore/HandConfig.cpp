// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/HandConfig.hpp>

#include <algorithm>
#include <cmath>

namespace OpenDesktopVR::Host::HandConfig {

using JsonUtil::Json;

HandSettings Read(const Json& settings) noexcept {
  HandSettings hands;
  const auto section = JsonUtil::GetOr<Json>(settings, "hands", Json {});
  hands.mEnabled = JsonUtil::GetOr<bool>(section, "enabled", true);
  hands.mShowWhilePointing = JsonUtil::GetOr<bool>(section, "showWhilePointing", true);
  const float offset = JsonUtil::GetOr<float>(section, "tapOffsetUpMillimeters", 0.0f);
  hands.mTapOffsetUpMillimeters = std::isfinite(offset)
    ? std::clamp(offset, -HandSettings::MaxTapOffsetMillimeters, HandSettings::MaxTapOffsetMillimeters)
    : 0.0f;
  hands.mReverseScroll = JsonUtil::GetOr<bool>(section, "reverseScroll", false);
  return hands;
}

void Write(Json& settings, const HandSettings& hands) {
  Json section = JsonUtil::GetOr<Json>(settings, "hands", Json::object());
  if (!section.is_object()) {
    section = Json::object();
  }
  section["enabled"] = hands.mEnabled;
  section["showWhilePointing"] = hands.mShowWhilePointing;
  section["tapOffsetUpMillimeters"] = hands.mTapOffsetUpMillimeters;
  section["reverseScroll"] = hands.mReverseScroll;
  settings["hands"] = std::move(section);
}

}// namespace OpenDesktopVR::Host::HandConfig
