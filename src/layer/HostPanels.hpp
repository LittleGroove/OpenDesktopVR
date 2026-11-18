// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The host's panels inside the game (doc 03 "xrEndFrame work", step 3).
// For each visible panel whose content changed (ADR-012 as refined by
// ADR-024; SY-1, SY-2):
// - newest ready (fence completed; a non-blocking read): copy, no wait;
// - otherwise the frame before it, if that's ready and newer than shown;
// - one frame behind: keep it (a normal, short lag);
// - nothing shown yet: show nothing until a frame is ready (never a wait);
// - two or more behind: keep the previous image up to 40 ms, then a
//   GPU-side wait, only while the host runs and its fence is moving.
// The "what's shown" record (SY-4) holds the host generation, texture
// generation and content version; it's reset on swapchain (re)creation and
// set only after a successful release.
// Swapchains are the size of the host's textures, which only grow (RN-7):
// each frame's own size (per ring slot) sets the copy and the quad's image
// rectangle, so a panel being resized or trimmed re-creates nothing.
#pragma once

#include "Dispatch.hpp"
#include "LayerStats.hpp"
#include "PanelSwapchain.hpp"

#include <OpenDesktopVR/D3D11Util.hpp>
#include <OpenDesktopVR/HostLink.hpp>
#include <OpenDesktopVR/Layer/Composition.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace OpenDesktopVR::Layer {

struct StalenessOptions {
  // ADR-012: a starting value, to be tuned from the stats line's counts
  std::chrono::milliseconds mMaxStaleness {40};
  // ADR-024: a GPU wait only on a fence that completed something this
  // recently, so it can't wait for a signal that never comes
  std::chrono::milliseconds mProgressWindow {250};
};

class HostPanels {
 public:
  HostPanels(
    const NextFunctions& next,
    XrSession session,
    const XrSpace& localSpace,
    ID3D11Device* device,
    ID3D11DeviceContext1* context,
    const D3D11Util::PrivateContextState& contextState,
    DXGI_FORMAT swapchainFormat,
    LayerStats& stats,
    StalenessOptions options = {});

  bool IsUsable() const noexcept {
    return mLink.IsUsable() && mWaitContext != nullptr;
  }

  // Brings visible panels up to date and appends their quads to `out`.
  // Returns how many were added. Never throws; allocates only when a panel
  // first appears or changes size.
  std::size_t Compose(TimeUtil::TimePoint now, std::span<ComposedPanel> out) noexcept;

  // The layer's report for the host (adapter, formats, counters, focus)
  void Report(SharedAbi::LayerStatus& status) noexcept;

  // Gaze focus (doc 05): of the panels the last Compose added, the one the
  // head points at (nearest, with a small tolerance; the focused one keeps
  // a wider one, so focus doesn't flicker at an edge). Marks it in
  // `panels`: it wins depth ties. True if it changed: report, then wake the
  // host, which draws it with its "looked at" opacity (PN-4).
  bool UpdateFocus(const std::optional<XrPosef>& head, std::span<ComposedPanel> panels) noexcept;
  uint64_t FocusedPanel() const noexcept {
    return mFocusedID;
  }
  void WakeHost() noexcept {
    mLink.WakeHost();
  }
  // The link itself, for hands (samples out, hand state in)
  HostLink::Client& Link() noexcept {
    return mLink;
  }

  struct Counters {
    uint64_t mCopies {0};
    uint64_t mKeptPrevious {0};
    uint64_t mForcedWaits {0};
    uint64_t mPreviousSlotCopies {0};// the newest wasn't done; the one before was
  };

  // True once after the link (re)connects: report at once, so the host can
  // match our format without waiting for the next once-a-second report
  bool TakeReportDue() noexcept {
    return std::exchange(mReportDue, false);
  }
  Counters GetCounters() const noexcept {
    return mCounters;
  }
  // Where the last Compose's time went (for the stats line)
  const FrameCosts& LastFrameCosts() const noexcept {
    return mCosts;
  }

 private:
  struct Record {
    uint64_t mPanelID {0};
    std::unique_ptr<PanelSwapchain> mSwapchain;
    uint64_t mShownGeneration {0};
    uint32_t mShownTextureGeneration {0};
    std::optional<TimeUtil::TimePoint> mLateSince;
    uint64_t mProgressGeneration {0};
    uint32_t mProgressTextureGeneration {0};
    uint64_t mLastCompleted {0};
    TimeUtil::TimePoint mLastProgress {};
    bool mLoggedStuck {false};
    XrCompositionLayerQuad mQuad {XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool mLoggedFormat {false};
  };

  Record* RecordFor(uint64_t panelID) noexcept;
  bool Bring(Record& record, const SharedAbi::PanelEntry& entry, const HostLink::PanelResources& resources, TimeUtil::TimePoint now, bool& createBudget) noexcept;

  const NextFunctions& mNext;
  XrSession mSession;
  // The session's panel space: re-made at each recenter (ADR-037)
  const XrSpace& mLocalSpace;
  ID3D11DeviceContext1* mContext;
  winrt::com_ptr<ID3D11DeviceContext4> mWaitContext;
  const D3D11Util::PrivateContextState& mContextState;
  DXGI_FORMAT mSwapchainFormat;
  DXGI_FORMAT mSwapchainTypeless {DXGI_FORMAT_UNKNOWN};
  LayerStats& mStats;
  StalenessOptions mOptions;
  HostLink::Client mLink;
  std::array<Record, SharedAbi::MaxPanels> mRecords;
  Counters mCounters;
  FrameCosts mCosts;
  // The panel ID behind each quad the last Compose added
  std::array<uint64_t, SharedAbi::MaxPanels> mComposedIDs {};
  uint64_t mFocusedID {0};
  bool mWasConnected {false};
  bool mReportDue {false};
};

}// namespace OpenDesktopVR::Layer
