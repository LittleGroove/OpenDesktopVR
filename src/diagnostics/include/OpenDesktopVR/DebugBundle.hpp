// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// "Save debug info" (doc 09 "Debug bundle"; LG-4): one .zip a user can send.
//
// In it:
// - README.txt: what's in it;
// - the texts the caller gives (the system, every OpenXR API layer, the
//   self-test, the in-game status, the recent log lines);
// - settings.json and the profiles, paths redacted (Redaction.hpp);
// - the logs (host and mirror, the newest layer logs), paths redacted;
// - crashes.txt and the newest crash dumps, as they are.
//
// Crash dumps and window titles can hold personal data: the settings window
// shows PrivacyNotice() before saving. Never throws; files that can't be
// read are listed in the README instead.
#pragma once

#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/Redaction.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenDesktopVR::Diagnostics {

struct BundleSources {
  std::filesystem::path mDataFolder;// settings.json, profiles\, logs\, crashdumps\ in it
  // Text files to add as they are named: ("system.txt", text), ...
  std::vector<std::pair<std::string, std::string>> mTexts;
  Redaction mRedaction;
  std::size_t mMaxDumps {5};
  std::size_t mMaxLayerLogs {10};
};

struct BundleResult {
  bool mOk {false};
  std::size_t mFiles {0};
  std::size_t mBytes {0};
  std::string mError;// when not ok
};

// Builds the bundle and writes it atomically to `zipPath`
BundleResult WriteBundle(const std::filesystem::path& zipPath, const BundleSources& sources) noexcept;

// Every API layer, in the loader's registry places, for the bundle (LG-4):
// where, on or off, whether its file exists, its name and DLL
std::string DescribeLayers(const std::vector<Registration::LayerEntry>& layers);

// Shown before saving (doc 09: "a short disclaimer")
std::string_view PrivacyNotice() noexcept;

// "OpenDesktopVR-debug-20261117-1405.zip"
std::wstring SuggestedBundleName();

}// namespace OpenDesktopVR::Diagnostics
