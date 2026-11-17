// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The "doesn't show in game" self-test (UI-10, LG-5; doc 09): each check
// passes or says what's wrong and how to fix it.
//
// - Our layer: registered and turned on, its manifest readable, no second
//   copy of ours turned on (CMP-3), on in settings and for the game.
// - A game: whether one loaded the layer, and when.
// - Layer order (CMP-1, CMP-2), from what the layer saw below it: Ultraleap
//   last; pose-changing and mirror layers below ours. Advice only: other
//   products' registry entries are never changed.
// - Depth submission (CMP-7).
// - Elevation and UAC (CMP-8): this app, Explorer, the game, UAC off.
// - Recenter: "Bring panels in front of me", and a hint to bind it.
//
// A pure function of what's gathered (SelfTestInput), so each verdict is
// tested without the PC being in that state.
#pragma once

#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::Diagnostics {

enum class Verdict { Pass, Info, Warning, Fail };
std::string_view VerdictName(Verdict verdict) noexcept;// "ok", "note", "warning", "problem"

enum class CheckID {
  Registered,
  Manifest,
  OtherCopies,
  OnInSettings,
  GameLoaded,
  Ultraleap,
  PoseChanging,
  MirrorCapture,
  Depth,
  HostElevation,
  ExplorerElevation,
  Uac,
  GameElevation,
  Recenter,
};

struct SelfCheck {
  CheckID mID {CheckID::Registered};
  std::string mTitle; // "The layer is registered"
  Verdict mVerdict {Verdict::Pass};
  std::string mDetail;// what was found
  std::string mFix;   // what to do; empty when there's nothing to do
};

// The game the layer last attached to, from its status (SharedAbi::LayerStatus)
struct GameSeen {
  std::string mExecutable;// "EliteDangerous64.exe"
  bool mRunning {false};
  std::chrono::seconds mAgo {0};// since it attached
  ProcessUtil::Elevation mElevation {ProcessUtil::Elevation::Unknown};
  uint32_t mChainFlags {0};// SharedAbi::ChainFlags; 0 from a layer before M6c
  uint32_t mLayersBelow {0};
  uint32_t mKnownBelow {0};// KnownLayers::Kind bits
  uint32_t mNearestRuntime {0};
};

struct SelfTestInput {
  std::vector<Registration::LayerEntry> mLayers;// Registration::ListAllImplicitLayers()
  std::filesystem::path mOurManifest;          // this install's
  bool mLayerOnInSettings {true};
  std::vector<std::wstring> mDisabledGames;
  std::optional<GameSeen> mGame;
  ProcessUtil::Elevation mHost {ProcessUtil::Elevation::NotElevated};
  ProcessUtil::Elevation mExplorer {ProcessUtil::Elevation::NotElevated};
  std::optional<bool> mUacOn;
  bool mRecenterBound {false};
};

std::vector<SelfCheck> RunSelfTest(const SelfTestInput& input);

// The checks as text, one block each, for the debug bundle and Copy
std::string FormatChecks(const std::vector<SelfCheck>& checks);

// "3 minutes ago", "just now", "2 hours ago"
std::string DescribeAgo(std::chrono::seconds ago);

}// namespace OpenDesktopVR::Diagnostics
