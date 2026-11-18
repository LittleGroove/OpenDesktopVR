// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The host's side of diagnostics and support (M6c, doc 09): gathering what
// the self-test, the Diagnostics tab and the debug bundle need from the
// running host, and saving the bundle. The logic and the bundle's format
// are in the diagnostics module; this is the glue.
//
// Threads: FromHost reads the panel manager, so it runs on the UI thread;
// AddSystem and SaveBundle read the registry and files, so they run on a
// worker.
#pragma once

#include "PanelManager.hpp"

#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/DebugBundle.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/SelfTest.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace OpenDesktopVR::Host::Support {

// The self-test's input from the settings and the layer's last report (UI thread)
Diagnostics::SelfTestInput FromHost(const RenderStats& stats, const PanelManager& panels);
// Then the registry, elevation and whether the game still runs (any thread)
void AddSystem(Diagnostics::SelfTestInput& input, uint32_t gameProcessID);

// The layer's last report as label and value rows: the game, its runtime,
// graphics API, hand tracking, depth, the layers below, frame counts
struct Row {
  std::string mLabel;
  std::string mValue;
};
std::vector<Row> DescribeGame(const RenderStats& stats);
// Each window panel's helper state (doc 09: "the helper's state per app")
std::vector<Row> DescribeHelpers(const PanelManager& panels, const AppHelper::AppHelperService& appHelper);
std::string FormatRows(const std::vector<Row>& rows);

// Everything gathered on the UI thread for a bundle
struct BundleRequest {
  std::filesystem::path mZip;
  Diagnostics::SelfTestInput mSelfTest;// from FromHost
  uint32_t mGameProcessID {0};
  std::string mStatus; // FormatRows(DescribeGame(...))
  std::string mHelpers;// FormatRows(DescribeHelpers(...))
  std::string mRecentLog;
};
// Writes the bundle (any thread; a few hundred milliseconds)
Diagnostics::BundleResult SaveBundle(BundleRequest request);

// This install's layer manifest: the one the host registers
std::filesystem::path OurManifest();

}// namespace OpenDesktopVR::Host::Support
