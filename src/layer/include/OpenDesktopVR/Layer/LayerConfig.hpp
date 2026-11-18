// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// What the layer reads at xrCreateInstance: whether it's on, whether it's off
// for this game (VR-8), and whether to show the built-in test panel.
// Read once per instance, never per frame. Corrupt or missing settings mean
// defaults (ST-4); the host owns writing them.
//
// settings.json:
//   "layer": {
//     "enabled": true,
//     "disabledGames": ["SomeGame.exe"],   // executable names, any case
//     "testPanel": false
//   }
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Layer {

struct LayerConfig {
  bool mEnabled {true};
  bool mTestPanel {false};
  bool mDisabledByEnvironment {false};
  std::vector<std::wstring> mDisabledGames;

  bool IsDisabledFor(std::wstring_view executableName) const noexcept;
};

LayerConfig ParseLayerConfig(std::string_view settingsJson) noexcept;

using GetEnvironment = std::function<std::optional<std::string>(std::string_view name)>;
void ApplyEnvironment(LayerConfig& config, const GetEnvironment& getEnvironment);

// From the product's settings file and this process's environment
LayerConfig LoadLayerConfig() noexcept;

}// namespace OpenDesktopVR::Layer
