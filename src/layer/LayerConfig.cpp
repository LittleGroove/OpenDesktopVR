// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/Layer/LayerConfig.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

namespace OpenDesktopVR::Layer {

bool LayerConfig::IsDisabledFor(std::wstring_view executableName) const noexcept {
  if (!mEnabled || mDisabledByEnvironment) {
    return true;
  }
  for (const auto& game: mDisabledGames) {
    if (StringUtil::EqualsIgnoreCase(game, executableName)) {
      return true;
    }
  }
  return false;
}

LayerConfig ParseLayerConfig(std::string_view settingsJson) noexcept {
  LayerConfig config;
  try {
    const auto settings = JsonUtil::Parse(settingsJson);
    if (!settings) {
      return config;
    }
    const auto layer = JsonUtil::Get<JsonUtil::Json>(*settings, "layer");
    if (!layer) {
      return config;
    }
    config.mEnabled = JsonUtil::GetOr<bool>(*layer, "enabled", true);
    config.mTestPanel = JsonUtil::GetOr<bool>(*layer, "testPanel", false);
    if (const auto games = JsonUtil::Get<JsonUtil::Json>(*layer, "disabledGames")) {
      config.mDisabledGames = JsonUtil::ParseEach<std::wstring>(
        *games, [](const JsonUtil::Json& entry) -> std::optional<std::wstring> {
          if (!entry.is_string()) {
            return std::nullopt;
          }
          auto name = StringUtil::Widen(entry.get<std::string>());
          return name.empty() ? std::nullopt : std::optional {std::move(name)};
        });
    }
  } catch (...) {
    return LayerConfig {};
  }
  return config;
}

void ApplyEnvironment(LayerConfig& config, const GetEnvironment& getEnvironment) {
  if (!getEnvironment) {
    return;
  }
  if (getEnvironment(Product::LayerDisableEnvVar)) {
    config.mDisabledByEnvironment = true;
  }
  if (const auto testPanel = getEnvironment(Product::TestPanelEnvVar)) {
    config.mTestPanel = (*testPanel != "0" && !testPanel->empty());
  }
}

LayerConfig LoadLayerConfig() noexcept {
  try {
    LayerConfig config;
    if (const auto file = Product::SettingsFile(); !file.empty()) {
      if (const auto text = FileUtil::ReadAll(file)) {
        config = ParseLayerConfig(*text);
      }
    }
    ApplyEnvironment(config, &ProcessUtil::GetEnvironment);
    return config;
  } catch (...) {
    return LayerConfig {};
  }
}

}// namespace OpenDesktopVR::Layer
