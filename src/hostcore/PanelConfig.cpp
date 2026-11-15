// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/PanelConfig.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <cmath>
#include <format>

namespace OpenDesktopVR::Host::PanelConfig {

using JsonUtil::Json;

namespace {

std::optional<uint64_t> ParseID(const std::string& text) {
  if (text.empty() || text.size() > 16) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char c: text) {
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= static_cast<uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= static_cast<uint64_t>(c - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return value == 0 ? std::nullopt : std::optional {value};
}

bool Finite(float v) {
  return std::isfinite(v);
}

}// namespace

std::optional<PanelDefinition> FromJson(const Json& json) noexcept {
  try {
    if (!json.is_object()) {
      return std::nullopt;
    }
    PanelDefinition panel;
    panel.mExtra = json;
    const auto id = ParseID(JsonUtil::GetOr<std::string>(json, "id", ""));
    if (!id) {
      return std::nullopt;
    }
    panel.mID = *id;
    panel.mName = StringUtil::Widen(JsonUtil::GetOr<std::string>(json, "name", ""));
    const auto kind = JsonUtil::GetOr<std::string>(json, "kind", "");
    if (kind == "window") {
      panel.mKind = PanelKind::Window;
      const auto rule = WindowMatch::FromJson(JsonUtil::GetOr<Json>(json, "window", Json {}));
      if (!rule) {
        return std::nullopt;
      }
      panel.mWindow = *rule;
    } else if (kind == "testPattern") {
      panel.mKind = PanelKind::TestPattern;
      panel.mAnimated = JsonUtil::GetOr<bool>(json, "animated", true);
    } else {
      return std::nullopt;// a kind this version doesn't have: skipped, kept on disk
    }
    if (const auto position = JsonUtil::Get<Json>(json, "position"); position && position->is_array() && position->size() == 3) {
      for (std::size_t i = 0; i < 3; ++i) {
        if (!(*position)[i].is_number()) {
          return std::nullopt;
        }
        panel.mPosition[i] = (*position)[i].get<float>();
      }
    }
    panel.mFaceOrigin = JsonUtil::GetOr<bool>(json, "faceOrigin", true);
    panel.mYawDegrees = JsonUtil::GetOr<float>(json, "yawDegrees", 0.0f);
    panel.mWidthMeters = JsonUtil::GetOr<float>(json, "widthMeters", 0.4f);
    panel.mMaxUpdatesPerSecond = JsonUtil::GetOr<double>(json, "maxUpdatesPerSecond", 0.0);
    panel.mEnabled = JsonUtil::GetOr<bool>(json, "enabled", true);
    // Untrusted (hand-edited files): finite numbers in sensible ranges
    if (!Finite(panel.mPosition[0]) || !Finite(panel.mPosition[1]) || !Finite(panel.mPosition[2])
        || std::abs(panel.mPosition[0]) > 20 || std::abs(panel.mPosition[1]) > 20 || std::abs(panel.mPosition[2]) > 20
        || !Finite(panel.mYawDegrees) || !Finite(panel.mWidthMeters) || panel.mWidthMeters < 0.02f || panel.mWidthMeters > 10
        || !std::isfinite(panel.mMaxUpdatesPerSecond) || panel.mMaxUpdatesPerSecond < 0) {
      return std::nullopt;
    }
    return panel;
  } catch (...) {
    return std::nullopt;
  }
}

Json ToJson(const PanelDefinition& panel) {
  // Start from what was read, so unknown fields survive (ST-5)
  Json json = panel.mExtra.is_object() ? panel.mExtra : Json::object();
  json["id"] = std::format("{:016x}", panel.mID);
  json["name"] = StringUtil::Narrow(panel.mName);
  if (panel.mKind == PanelKind::Window) {
    json["kind"] = "window";
    json["window"] = WindowMatch::ToJson(panel.mWindow);
    json.erase("animated");
  } else {
    json["kind"] = "testPattern";
    json["animated"] = panel.mAnimated;
    json.erase("window");
  }
  json["position"] = Json::array({panel.mPosition[0], panel.mPosition[1], panel.mPosition[2]});
  json["faceOrigin"] = panel.mFaceOrigin;
  json["yawDegrees"] = panel.mYawDegrees;
  json["widthMeters"] = panel.mWidthMeters;
  json["maxUpdatesPerSecond"] = panel.mMaxUpdatesPerSecond;
  json["enabled"] = panel.mEnabled;
  return json;
}

std::optional<std::vector<PanelDefinition>> Read(const Json& settings, std::size_t* skipped) {
  const auto list = JsonUtil::Get<Json>(settings, "panels");
  if (!list || !list->is_array()) {
    if (skipped) {
      *skipped = 0;
    }
    return std::nullopt;
  }
  return JsonUtil::ParseEach<PanelDefinition>(*list, [](const Json& entry) { return FromJson(entry); }, skipped);
}

void Write(Json& settings, std::span<const PanelDefinition> panels) {
  // Entries this version couldn't read stay in the list, untouched (ST-5)
  Json list = Json::array();
  for (const auto& panel: panels) {
    list.push_back(ToJson(panel));
  }
  if (const auto old = JsonUtil::Get<Json>(settings, "panels"); old && old->is_array()) {
    for (const auto& entry: *old) {
      if (!FromJson(entry)) {
        list.push_back(entry);
      }
    }
  }
  settings["panels"] = std::move(list);
}

}// namespace OpenDesktopVR::Host::PanelConfig
