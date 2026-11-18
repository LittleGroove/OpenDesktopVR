// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include "Support.hpp"

#include <OpenDesktopVR/KnownLayers.hpp>
#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/SystemInfo.hpp>

#include <Windows.h>

#include <format>

namespace OpenDesktopVR::Host::Support {

namespace {

std::string Field(const char* text, std::size_t size) {
  return {text, strnlen(text, size)};
}

std::string_view GraphicsApiName(uint32_t api) {
  switch (static_cast<SharedAbi::GraphicsApi>(api)) {
    case SharedAbi::GraphicsApi::D3D11:
      return "Direct3D 11";
    case SharedAbi::GraphicsApi::D3D12:
      return "Direct3D 12";
    case SharedAbi::GraphicsApi::Vulkan:
      return "Vulkan";
    case SharedAbi::GraphicsApi::OpenGL:
      return "OpenGL";
    default:
      return "unknown";
  }
}

// XrSessionState's values (OpenXR 1.0)
std::string_view SessionStateName(uint32_t state) {
  constexpr std::string_view Names[] {
    "unknown", "idle", "ready", "synchronized", "visible", "focused", "stopping", "loss pending", "exiting"};
  return state < std::size(Names) ? Names[state] : "unknown";
}

std::string KindsBelow(uint32_t known) {
  std::string text;
  for (const auto kind: {KnownLayers::Ultraleap, KnownLayers::PoseChanging, KnownLayers::MirrorCapture}) {
    if (known & kind) {
      text += text.empty() ? "" : ", ";
      text += KnownLayers::Describe(kind);
    }
  }
  return text;
}

ProcessUtil::Elevation ExplorerElevation() {
  DWORD explorer = 0;
  if (const HWND shell = GetShellWindow()) {
    GetWindowThreadProcessId(shell, &explorer);
  }
  return explorer ? ProcessUtil::ProcessElevation(explorer) : ProcessUtil::Elevation::Unknown;
}

}// namespace

std::filesystem::path OurManifest() {
  return ProcessUtil::ExecutablePath().parent_path() / Product::LayerManifestName;
}

Diagnostics::SelfTestInput FromHost(const RenderStats& stats, const PanelManager& panels) {
  Diagnostics::SelfTestInput input;
  const auto games = panels.Games();
  input.mLayerOnInSettings = games.mLayerEnabled;
  input.mDisabledGames = games.mDisabledGames;
  const auto& recenter = panels.GlobalKeybinds(Keybinds::ActionKind::Recenter);
  input.mRecenterBound = std::ranges::any_of(recenter, [](const Keybinds::Binding& binding) { return !binding.IsEmpty(); });
  if (const auto& status = stats.mLayerStatus; status && status->mLayerProcessID != 0) {
    Diagnostics::GameSeen game;
    game.mExecutable = Field(status->mExecutable, sizeof(status->mExecutable));
    game.mAgo = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - stats.mLayerAttachedAt);
    game.mChainFlags = status->mChainFlags;
    game.mLayersBelow = status->mLayersBelow;
    game.mKnownBelow = status->mKnownBelow;
    game.mNearestRuntime = status->mNearestRuntime;
    input.mGame = game;
  }
  return input;
}

void AddSystem(Diagnostics::SelfTestInput& input, uint32_t gameProcessID) {
  input.mLayers = Registration::ListAllImplicitLayers();
  input.mOurManifest = OurManifest();
  input.mHost = ProcessUtil::CurrentProcessElevation();
  input.mExplorer = ExplorerElevation();
  input.mUacOn = ProcessUtil::UacEnabled();
  if (input.mGame) {
    input.mGame->mRunning = ProcessUtil::IsRunning(gameProcessID);
    input.mGame->mElevation = input.mGame->mRunning ? ProcessUtil::ProcessElevation(gameProcessID) : ProcessUtil::Elevation::Unknown;
  }
}

std::vector<Row> DescribeGame(const RenderStats& stats) {
  std::vector<Row> rows;
  const auto& status = stats.mLayerStatus;
  if (!status || status->mLayerProcessID == 0) {
    return rows;
  }
  const bool running = ProcessUtil::IsRunning(status->mLayerProcessID);
  const auto ago = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - stats.mLayerAttachedAt);
  rows.push_back({"Game", std::format("{} (process {}{})", Field(status->mExecutable, sizeof(status->mExecutable)),
                            status->mLayerProcessID, running ? "" : "; ended")});
  rows.push_back({"Attached", Diagnostics::DescribeAgo(ago)});
  rows.push_back({"Application", Field(status->mApplication, sizeof(status->mApplication))});
  rows.push_back({"Runtime", Field(status->mRuntime, sizeof(status->mRuntime))});
  rows.push_back({"Graphics", std::string {GraphicsApiName(status->mGraphicsApi)}});
  rows.push_back({"Session", std::string {SessionStateName(status->mSessionState)}});
  rows.push_back({"Hand tracking", status->mHandTrackingAvailable ? "available" : "not available"});
  const bool known = (status->mChainFlags & SharedAbi::ChainKnown) != 0;
  rows.push_back({"Depth submission", !known ? "unknown (an older layer)" : ((status->mChainFlags & SharedAbi::DepthEnabled) ? "turned on by the game" : "off")});
  if (known) {
    const auto kinds = KindsBelow(status->mKnownBelow);
    rows.push_back({"Layers below ours", std::format("{}{}", status->mLayersBelow, kinds.empty() ? "" : std::format(" (including {})", kinds))});
  }
  rows.push_back({"Frames", std::format("{}", status->mFramesSinceAttach)});
  rows.push_back({"Panel images copied", std::format("{}", status->mPanelsCopied)});
  rows.push_back({"Frames that kept an older image", std::format("{}", status->mKeptPrevious)});
  rows.push_back({"Waits for late panels", std::format("{}", status->mForcedWaits)});
  if (status->mLastError != 0) {
    rows.push_back({"Last OpenXR error", std::format("{}", status->mLastError)});
  }
  return rows;
}

std::vector<Row> DescribeHelpers(const PanelManager& panels, const AppHelper::AppHelperService& appHelper) {
  std::vector<Row> rows;
  for (const auto& panel: panels.Panels()) {
    if (panel.mKind != PanelKind::Window) {
      continue;
    }
    std::string text;
    if (const auto status = appHelper.StatusOf(panel.mID)) {
      text = status->mText;
    }
    // One helper does both; its two states read alike once it's loaded
    // (round 18 showed the same sentence twice)
    if (const auto input = appHelper.InputStatusOf(panel.mID); input && !input->mText.empty() && input->mText != text) {
      text = text.empty() ? input->mText : std::format("{} Taps: {}", text, input->mText);
    }
    rows.push_back({StringUtil::Narrow(panel.mName.empty() ? L"(no name)" : panel.mName), text.empty() ? "not showing yet" : text});
  }
  return rows;
}

std::string FormatRows(const std::vector<Row>& rows) {
  std::string text;
  for (const auto& row: rows) {
    text += std::format("{}: {}\r\n", row.mLabel, row.mValue);
  }
  return text;
}

Diagnostics::BundleResult SaveBundle(BundleRequest request) {
  AddSystem(request.mSelfTest, request.mGameProcessID);
  Diagnostics::BundleSources sources;
  sources.mDataFolder = Product::DataFolder();
  sources.mRedaction = Diagnostics::Redaction::ForThisUser();
  sources.mTexts = {
    {"system.txt", Diagnostics::DescribeSystem()},
    {"openxr-layers.txt", Diagnostics::DescribeLayers(request.mSelfTest.mLayers)},
    {"self-test.txt", Diagnostics::FormatChecks(Diagnostics::RunSelfTest(request.mSelfTest))},
    {"in-game.txt", request.mStatus.empty() ? std::string {"No game has loaded the layer since Open Desktop VR started.\r\n"} : request.mStatus},
    {"apps.txt", request.mHelpers.empty() ? std::string {"No window panels.\r\n"} : request.mHelpers},
    {"log-recent.txt", request.mRecentLog},
  };
  const auto result = Diagnostics::WriteBundle(request.mZip, sources);
  if (result.mOk) {
    Log::Info("Debug info saved: {} files, {} KB", result.mFiles, result.mBytes / 1024);
  } else {
    Log::Warning("Debug info couldn't be saved: {}", result.mError);
  }
  return result;
}

}// namespace OpenDesktopVR::Host::Support
