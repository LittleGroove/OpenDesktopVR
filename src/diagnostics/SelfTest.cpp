// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/KnownLayers.hpp>
#include <OpenDesktopVR/SelfTest.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>
#include <format>

namespace OpenDesktopVR::Diagnostics {

namespace {

using Elevation = ProcessUtil::Elevation;
using Registration::LayerEntry;

constexpr std::string_view OrderTool =
  "Use a layer-ordering tool to change the order. Open Desktop VR never changes other products' settings.";

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
  std::error_code error;
  if (!a.empty() && !b.empty() && std::filesystem::equivalent(a, b, error)) {
    return true;
  }
  return StringUtil::EqualsIgnoreCase(a.lexically_normal().wstring(), b.lexically_normal().wstring());
}

std::string Where(const LayerEntry& entry) {
  return std::format("{}: {}", StringUtil::Narrow(entry.mWhere), StringUtil::Narrow(entry.mManifestPath.wstring()));
}

uint32_t KindOf(const LayerEntry& entry) {
  if (entry.mIsOurs) {
    return KnownLayers::None;
  }
  const auto byName = entry.mLayerName ? KnownLayers::Classify(*entry.mLayerName) : KnownLayers::None;
  return byName != KnownLayers::None ? byName : KnownLayers::Classify(StringUtil::Narrow(entry.mManifestPath.wstring()));
}

const LayerEntry* FirstEnabledOfKind(const std::vector<LayerEntry>& layers, uint32_t kind) {
  const auto it = std::ranges::find_if(layers, [kind](const LayerEntry& entry) { return entry.mEnabled && KindOf(entry) == kind; });
  return it == layers.end() ? nullptr : &*it;
}

std::string NameOf(const LayerEntry& entry) {
  return entry.mLayerName.value_or(StringUtil::Narrow(entry.mManifestPath.filename().wstring()));
}

bool ChainKnown(const SelfTestInput& input) {
  return input.mGame && (input.mGame->mChainFlags & SharedAbi::ChainKnown) != 0;
}

bool GameIsDisabled(const SelfTestInput& input) {
  if (!input.mGame) {
    return false;
  }
  const auto exe = StringUtil::Widen(input.mGame->mExecutable);
  return std::ranges::any_of(input.mDisabledGames, [&exe](const std::wstring& game) { return StringUtil::EqualsIgnoreCase(game, exe); });
}

// Ultraleap, pose-changing and mirror layers: where they sit, from the chain
void OrderCheck(std::vector<SelfCheck>& checks, const SelfTestInput& input, CheckID id, uint32_t kind) {
  const auto* entry = FirstEnabledOfKind(input.mLayers, kind);
  if (!entry) {
    return;// not installed (or off): nothing to say
  }
  const auto name = NameOf(*entry);
  SelfCheck check {id};
  switch (kind) {
    case KnownLayers::Ultraleap:
      check.mTitle = "Layer order: Ultraleap";
      break;
    case KnownLayers::PoseChanging:
      check.mTitle = "Layer order: pose-changing layers";
      break;
    default:
      check.mTitle = "Layer order: mirror and capture layers";
      break;
  }
  if (!ChainKnown(input)) {
    check.mVerdict = Verdict::Info;
    check.mDetail = std::format("{} is installed. Where it sits shows once a game has run with Open Desktop VR.", name);
    if (kind == KnownLayers::Ultraleap) {
      check.mFix = "If panels never show in games, Ultraleap's layer may be above ours: it must be last, nearest the runtime.";
    }
    checks.push_back(std::move(check));
    return;
  }
  const auto& game = *input.mGame;
  const bool below = (game.mKnownBelow & kind) != 0;
  if (kind == KnownLayers::Ultraleap) {
    if (below && game.mNearestRuntime == KnownLayers::Ultraleap) {
      check.mDetail = std::format("{} is last, nearest the runtime, as it must be.", name);
    } else if (below) {
      check.mVerdict = Verdict::Warning;
      check.mDetail = std::format("{} is below ours but not last. Layers below it don't work.", name);
      check.mFix = std::format("Move Ultraleap's layer to the bottom of the order. {}", OrderTool);
    } else {
      check.mVerdict = Verdict::Fail;
      check.mDetail = std::format("{} is above ours. It doesn't pass calls on to the layers below it, so our panels and hands can't show.", name);
      check.mFix = std::format("Move Ultraleap's layer to the bottom of the order, nearest the runtime. {}", OrderTool);
    }
  } else if (kind == KnownLayers::PoseChanging) {
    if (below) {
      check.mDetail = std::format("{} is below ours: panels follow the view it adjusts.", name);
    } else {
      check.mVerdict = Verdict::Warning;
      check.mDetail = std::format("{} is above ours: panels stay fixed to the room instead of following the view it adjusts.", name);
      check.mFix = std::format("Move it below ours, nearer the runtime. {}", OrderTool);
    }
  } else {
    if (below) {
      check.mDetail = std::format("{} is below ours: what it records includes the panels.", name);
    } else {
      check.mVerdict = Verdict::Info;
      check.mDetail = std::format("{} is above ours: what it records won't include the panels.", name);
      check.mFix = std::format("If recordings should show the panels, move it below ours. {}", OrderTool);
    }
  }
  checks.push_back(std::move(check));
}

}// namespace

std::string_view VerdictName(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::Pass:
      return "ok";
    case Verdict::Info:
      return "note";
    case Verdict::Warning:
      return "warning";
    case Verdict::Fail:
      return "problem";
  }
  return "?";
}

std::string DescribeAgo(std::chrono::seconds ago) {
  const auto seconds = ago.count();
  if (seconds < 60) {
    return "just now";
  }
  if (seconds < 3600) {
    const auto minutes = seconds / 60;
    return std::format("{} minute{} ago", minutes, minutes == 1 ? "" : "s");
  }
  const auto hours = seconds / 3600;
  return std::format("{} hour{} ago", hours, hours == 1 ? "" : "s");
}

std::vector<SelfCheck> RunSelfTest(const SelfTestInput& input) {
  std::vector<SelfCheck> checks;

  // ---- Our layer --------------------------------------------------------
  const auto ours = std::ranges::find_if(input.mLayers, [&input](const LayerEntry& entry) {
    return SamePath(entry.mManifestPath, input.mOurManifest);
  });
  {
    SelfCheck check {CheckID::Registered, "The layer is registered"};
    if (ours == input.mLayers.end()) {
      check.mVerdict = Verdict::Fail;
      check.mDetail = "Games can't find Open Desktop VR's layer: it isn't in the registry.";
      check.mFix = "Exit Open Desktop VR and start it again: it registers the layer as it starts.";
    } else if (!ours->mEnabled) {
      check.mVerdict = Verdict::Fail;
      check.mDetail = std::format("It's registered but turned off ({}).", Where(*ours));
      check.mFix = "Turn it on in the layer-ordering tool that turned it off, or restart Open Desktop VR.";
    } else {
      check.mDetail = std::format("Registered and on ({}).", Where(*ours));
    }
    checks.push_back(std::move(check));
  }
  if (ours != input.mLayers.end()) {
    SelfCheck check {CheckID::Manifest, "Its manifest file is readable"};
    if (!ours->mManifestExists || !ours->mLayerName) {
      check.mVerdict = Verdict::Fail;
      check.mDetail = ours->mManifestExists ? "The manifest file can't be read." : "The manifest file is missing.";
      check.mFix = "Install Open Desktop VR again.";
    } else {
      check.mDetail = std::format("{} names {}.", StringUtil::Narrow(ours->mManifestPath.filename().wstring()), *ours->mLayerName);
    }
    checks.push_back(std::move(check));
  }
  {
    std::vector<const LayerEntry*> others;
    for (const auto& entry: input.mLayers) {
      if (entry.mIsOurs && entry.mEnabled && !SamePath(entry.mManifestPath, input.mOurManifest)) {
        others.push_back(&entry);
      }
    }
    if (!others.empty()) {
      SelfCheck check {CheckID::OtherCopies, "Only one copy of the layer is on", Verdict::Warning};
      check.mDetail = "Another copy of Open Desktop VR's layer is turned on:";
      for (const auto* other: others) {
        check.mDetail += " " + Where(*other) + ".";
      }
      check.mFix = "Two copies must never load. Uninstall the other copy, or turn its entry off (set it to 1).";
      checks.push_back(std::move(check));
    }
  }
  {
    SelfCheck check {CheckID::OnInSettings, "Open Desktop VR is on in games"};
    if (!input.mLayerOnInSettings) {
      check.mVerdict = Verdict::Fail;
      check.mDetail = "It's turned off for all games.";
      check.mFix = "On the General tab, tick \"Use Open Desktop VR in games\", then restart the game.";
    } else if (GameIsDisabled(input)) {
      check.mVerdict = Verdict::Fail;
      check.mDetail = std::format("It's turned off for {}.", input.mGame->mExecutable);
      check.mFix = std::format("On the General tab, under Games, remove {} from the list, then restart the game.", input.mGame->mExecutable);
    } else {
      check.mDetail = input.mDisabledGames.empty() ? "On in every game." : std::format("On, except in {} listed games.", input.mDisabledGames.size());
    }
    checks.push_back(std::move(check));
  }

  // ---- A game -----------------------------------------------------------
  {
    SelfCheck check {CheckID::GameLoaded, "A game loaded the layer"};
    if (!input.mGame) {
      check.mVerdict = Verdict::Warning;
      check.mDetail = "No game has loaded the layer since Open Desktop VR started.";
      check.mFix =
        "Start Open Desktop VR before the game; if the game is running, restart it. Only OpenXR games load the "
        "layer: a game using SteamVR's own interface (OpenVR) doesn't.";
    } else {
      check.mDetail = std::format(
        "{} loaded it {}{}.",
        input.mGame->mExecutable,
        DescribeAgo(input.mGame->mAgo),
        input.mGame->mRunning ? "" : "; it has ended since");
    }
    checks.push_back(std::move(check));
  }

  // ---- Layer order (CMP-1, CMP-2) ---------------------------------------
  OrderCheck(checks, input, CheckID::Ultraleap, KnownLayers::Ultraleap);
  OrderCheck(checks, input, CheckID::PoseChanging, KnownLayers::PoseChanging);
  OrderCheck(checks, input, CheckID::MirrorCapture, KnownLayers::MirrorCapture);

  // ---- Depth (CMP-7) ----------------------------------------------------
  if (ChainKnown(input)) {
    SelfCheck check {CheckID::Depth, "Depth submission"};
    if (input.mGame->mChainFlags & SharedAbi::DepthEnabled) {
      check.mVerdict = Verdict::Warning;
      check.mDetail = std::format(
        "{} turns on depth submission. With it, some runtimes hide overlays behind the game, or show them with "
        "artifacts.",
        input.mGame->mExecutable);
      check.mFix = "If panels vanish behind the cockpit, turn off depth submission in the game's settings, if it has one.";
    } else {
      check.mDetail = std::format("{} doesn't submit depth.", input.mGame->mExecutable);
    }
    checks.push_back(std::move(check));
  }

  // ---- Elevation (CMP-8) ------------------------------------------------
  {
    SelfCheck check {CheckID::HostElevation, "Open Desktop VR runs as a normal app"};
    if (input.mHost == Elevation::Elevated) {
      check.mVerdict = Verdict::Warning;
      check.mDetail =
        "Open Desktop VR runs as administrator. A game that doesn't may not show the panels, and everything "
        "it opens runs as administrator too.";
      check.mFix =
        "Start Open Desktop VR normally: not with \"Run as administrator\", and not from a launcher that runs as "
        "administrator (Stream Deck, VoiceAttack and similar pass it on).";
    } else {
      check.mDetail = "It runs without administrator rights, as it should.";
    }
    checks.push_back(std::move(check));
  }
  if (input.mExplorer == Elevation::Elevated) {
    SelfCheck check {CheckID::ExplorerElevation, "Windows Explorer runs as a normal app", Verdict::Warning};
    check.mDetail = "Explorer runs as administrator, so everything started from the desktop or Start menu does too.";
    check.mFix = "Sign out and in again. If it stays elevated, UAC may be off or changed by a tweak tool.";
    checks.push_back(std::move(check));
  }
  if (input.mUacOn && !*input.mUacOn) {
    SelfCheck check {CheckID::Uac, "User Account Control is on", Verdict::Warning};
    check.mDetail = "UAC is off: every app runs with full administrator rights, which causes the kinds of problems above.";
    check.mFix = "Turn UAC back on: Start, type \"UAC\", open \"Change User Account Control settings\", then restart.";
    checks.push_back(std::move(check));
  }
  if (input.mGame && input.mGame->mRunning) {
    SelfCheck check {CheckID::GameElevation, "The game and Open Desktop VR run the same way"};
    const bool gameElevated = input.mGame->mElevation != Elevation::NotElevated;// unknown: probably elevated
    const bool hostElevated = input.mHost == Elevation::Elevated;
    if (gameElevated && !hostElevated) {
      check.mVerdict = Verdict::Warning;
      check.mDetail = std::format(
        "{} runs as administrator{} and Open Desktop VR doesn't. Windows keeps them apart, which can stop "
        "keybinds and panels from working in the game.",
        input.mGame->mExecutable,
        input.mGame->mElevation == Elevation::Unknown ? " (probably: it can't be checked)" : "");
      check.mFix =
        "Start the game normally: not as administrator, and not from a launcher that runs as administrator "
        "(Stream Deck, VoiceAttack and similar pass it on).";
    } else if (!gameElevated && hostElevated) {
      check.mVerdict = Verdict::Warning;
      check.mDetail = std::format("Open Desktop VR runs as administrator and {} doesn't.", input.mGame->mExecutable);
      check.mFix = "Start Open Desktop VR normally (see above).";
    } else {
      check.mDetail = std::format("{} runs the same way as Open Desktop VR.", input.mGame->mExecutable);
    }
    checks.push_back(std::move(check));
  }

  // ---- Recenter ---------------------------------------------------------
  {
    SelfCheck check {CheckID::Recenter, "Panels are in front of you", Verdict::Info};
    check.mDetail = "If panels are behind you or far away, \"Bring panels in front of me\" places them from where you're looking.";
    if (!input.mRecenterBound) {
      check.mFix = "To do it from inside the game, bind Recenter on the Keybinds tab.";
    }
    checks.push_back(std::move(check));
  }
  return checks;
}

std::string FormatChecks(const std::vector<SelfCheck>& checks) {
  std::string text;
  for (const auto& check: checks) {
    text += std::format("[{}] {}\r\n    {}\r\n", VerdictName(check.mVerdict), check.mTitle, check.mDetail);
    if (!check.mFix.empty()) {
      text += std::format("    Fix: {}\r\n", check.mFix);
    }
  }
  return text;
}

}// namespace OpenDesktopVR::Diagnostics
