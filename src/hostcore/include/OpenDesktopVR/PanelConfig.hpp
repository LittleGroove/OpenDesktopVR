// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Panels in settings.json (doc 09 "Settings storage"):
//
//   "panels": [
//     {"id": "1a2b...", "name": "Discord", "kind": "window",
//      "window": {"executable": "...", "class": "...", "title": "Discord", "titleMode": "contains"},
//      "position": [x, y, z], "faceOrigin": true, "yawDegrees": 0,
//      "widthMeters": 0.45, "maxUpdatesPerSecond": 0, "enabled": true},
//     {"id": "...", "name": "...", "kind": "testPattern", "animated": true, ...}
//   ]
//
// Reading is tolerant: one bad entry is skipped, the rest load (ST-4), and
// fields this version doesn't know survive a save (ST-5).
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>

#include <optional>
#include <span>
#include <vector>

namespace OpenDesktopVR::Host::PanelConfig {

// nullopt if settings have no "panels" list yet (a fresh install)
std::optional<std::vector<PanelDefinition>> Read(const JsonUtil::Json& settings, std::size_t* skipped = nullptr);

// Replaces the "panels" list, leaving everything else in `settings` alone
// The global keybinds ("keybinds" in settings; per-panel ones are in each
// panel's entry). Untrusted: malformed slots read as empty.
Keybinds::Slots ReadToggleAllKeybinds(const JsonUtil::Json& settings);
void WriteToggleAllKeybinds(JsonUtil::Json& settings, const Keybinds::Slots& slots);

void Write(JsonUtil::Json& settings, std::span<const PanelDefinition> panels);

std::optional<PanelDefinition> FromJson(const JsonUtil::Json& json) noexcept;
JsonUtil::Json ToJson(const PanelDefinition& panel);

}// namespace OpenDesktopVR::Host::PanelConfig
