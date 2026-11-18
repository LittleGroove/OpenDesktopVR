// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// odvr-layer: register the layer and change its settings from a terminal,
// until the host app does this itself (M2).
//
//   odvr-layer register [manifest]   register for this user (default: the
//                                     manifest next to this program)
//   odvr-layer unregister            remove our registry entries
//   odvr-layer list                  every implicit OpenXR layer, all hives
//   odvr-layer test-panel on|off     show the built-in test panel in games
//   odvr-layer disable-game <exe>    turn the layer off for one game
//   odvr-layer enable-game <exe>     turn it back on for that game

#include <OpenDesktopVR/GlobalSettings.hpp>
#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

using namespace OpenDesktopVR;
namespace fs = std::filesystem;

namespace {

int Usage() {
  std::printf(
    "Usage:\n"
    "  odvr-layer register [manifest]\n"
    "  odvr-layer unregister\n"
    "  odvr-layer list\n"
    "  odvr-layer test-panel on|off\n"
    "  odvr-layer disable-game <exe name>\n"
    "  odvr-layer enable-game <exe name>\n");
  return 2;
}

Settings::DocumentSpec SettingsSpec() {
  return Settings::GlobalSettingsSpec();// shared with the host (ADR-038)
}

// Edits the "layer" object of settings.json, keeping everything else
template <class Edit>
int EditLayerSettings(Edit&& edit) {
  if (Product::SettingsFile().empty()) {
    std::printf("Can't find the settings folder\n");
    return 1;
  }
  Settings::Store store;
  auto loaded = store.Load(SettingsSpec());
  auto& document = loaded.mDocument;
  if (!document.contains("layer") || !document["layer"].is_object()) {
    document["layer"] = Settings::Json::object();
  }
  edit(document["layer"]);
  store.Save(Product::SettingsFile(), document);
  store.Flush();
  std::printf("Saved %s\n", StringUtil::Narrow(Product::SettingsFile().wstring()).c_str());
  return 0;
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    return Usage();
  }
  const std::wstring_view command {argv[1]};

  if (command == L"register") {
    const fs::path manifest = argc > 2 ? fs::path {argv[2]} : ProcessUtil::ExecutablePath().parent_path() / Product::LayerManifestName;
    const auto result = Registration::Register(manifest);
    if (!result.mOk) {
      std::printf(
        "Couldn't register %s (error %ld)\n", StringUtil::Narrow(manifest.wstring()).c_str(), result.mError);
      return 1;
    }
    std::printf(
      "Registered %s for this user%s\n",
      StringUtil::Narrow(manifest.wstring()).c_str(),
      result.mStaleDisabled ? "; older entries of ours were disabled" : "");
    return 0;
  }

  if (command == L"unregister") {
    std::printf("Removed %d registry entr%s\n", Registration::Unregister(), "y/ies");
    return 0;
  }

  if (command == L"list") {
    const auto layers = Registration::ListAllImplicitLayers();
    if (layers.empty()) {
      std::printf("No implicit OpenXR layers are registered\n");
    }
    for (const auto& layer: layers) {
      std::printf(
        "%-15s %-8s %-4s %-40s %s%s\n",
        StringUtil::Narrow(layer.mWhere).c_str(),
        layer.mEnabled ? "enabled" : "disabled",
        layer.mIsOurs ? "ours" : "",
        layer.mLayerName.value_or("(unreadable manifest)").c_str(),
        StringUtil::Narrow(layer.mManifestPath.wstring()).c_str(),
        layer.mManifestExists ? "" : "  [MISSING]");
    }
    return 0;
  }

  if (command == L"test-panel" && argc > 2) {
    const std::wstring_view value {argv[2]};
    if (value != L"on" && value != L"off") {
      return Usage();
    }
    return EditLayerSettings([&](Settings::Json& layer) { layer["testPanel"] = (value == L"on"); });
  }

  if ((command == L"disable-game" || command == L"enable-game") && argc > 2) {
    const auto game = StringUtil::Narrow(argv[2]);
    const bool disable = command == L"disable-game";
    return EditLayerSettings([&](Settings::Json& layer) {
      auto games = layer.contains("disabledGames") && layer["disabledGames"].is_array() ? layer["disabledGames"]
                                                                                         : Settings::Json::array();
      Settings::Json kept = Settings::Json::array();
      for (const auto& entry: games) {
        const bool same = entry.is_string()
          && StringUtil::EqualsIgnoreCase(StringUtil::Widen(entry.get<std::string>()), StringUtil::Widen(game));
        if (!same) {
          kept.push_back(entry);
        }
      }
      if (disable) {
        kept.push_back(game);
      }
      layer["disabledGames"] = kept;
    });
  }

  return Usage();
}
