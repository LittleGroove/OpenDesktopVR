// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The host's panels inside the game (doc 03 "xrEndFrame work", step 3).
// For each visible panel whose content changed (ADR-012, SY-1, SY-2):
// - ready (fence completed; a non-blocking read): copy, no GPU wait;
// - not ready, previous image shown: keep it, for up to 40 ms;
// - still late after 40 ms, or nothing shown yet: queue a GPU-side wait on
//   the fence, then copy.
// The "what's shown" record (SY-4) holds the host generation, texture
// generation and content version; it's reset on swapchain (re)creation and
// set only after a successful release.
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

namespace OpenDesktopVR::Layer {

struct StalenessOptions {
  // ADR-012: a starting value, to be tuned from the stats line's counts
  std::chrono::milliseconds mMaxStaleness {40};
};

class HostPanels {
 public:
  HostPanels(
    const NextFunctions& next,
    XrSession session,
    XrSpace localSpace,
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

  // The layer's report for the host (adapter, formats, counters)
  void Report(SharedAbi::LayerStatus& status) noexcept;

  struct Counters {
    uint64_t mCopies {0};
    uint64_t mKeptPrevious {0};
    uint64_t mForcedWaits {0};
  };
  Counters GetCounters() const noexcept {
    return mCounters;
  }

 private:
  struct Record {
    uint64_t mPanelID {0};
    std::unique_ptr<PanelSwapchain> mSwapchain;
    uint64_t mShownGeneration {0};
    uint32_t mShownTextureGeneration {0};
    std::optional<TimeUtil::TimePoint> mLateSince;
    XrCompositionLayerQuad mQuad {XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool mLoggedFormat {false};
  };

  Record* RecordFor(uint64_t panelID) noexcept;
  bool Bring(Record& record, const SharedAbi::PanelEntry& entry, const HostLink::PanelResources& resources, TimeUtil::TimePoint now) noexcept;

  const NextFunctions& mNext;
  XrSession mSession;
  XrSpace mLocalSpace;
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
};

}// namespace OpenDesktopVR::Layer
