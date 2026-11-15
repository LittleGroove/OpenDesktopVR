// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "HostPanels.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

#include <cmath>

namespace OpenDesktopVR::Layer {

namespace {

// Everything from the host is untrusted (TH-9)
bool PlausibleEntry(const SharedAbi::PanelEntry& entry) {
  for (const float v: entry.mPose) {
    if (!std::isfinite(v)) {
      return false;
    }
  }
  const float w = entry.mSizeMeters[0];
  const float h = entry.mSizeMeters[1];
  return std::isfinite(w) && std::isfinite(h) && w > 0.01f && h > 0.01f && w < 50 && h < 50
    && entry.mContentWidth > 0 && entry.mContentHeight > 0 && entry.mContentWidth <= 16384
    && entry.mContentHeight <= 16384 && entry.mContentWidth <= entry.mTextureWidth
    && entry.mContentHeight <= entry.mTextureHeight && entry.mCurrentSlot < SharedAbi::RingSlots;
}

}// namespace

HostPanels::HostPanels(
  const NextFunctions& next,
  XrSession session,
  XrSpace localSpace,
  ID3D11Device* device,
  ID3D11DeviceContext1* context,
  const D3D11Util::PrivateContextState& contextState,
  DXGI_FORMAT swapchainFormat,
  LayerStats& stats,
  StalenessOptions options)
  : mNext(next),
    mSession(session),
    mLocalSpace(localSpace),
    mContext(context),
    mContextState(contextState),
    mSwapchainFormat(swapchainFormat),
    mStats(stats),
    mOptions(options),
    mLink(device) {
  if (context) {
    context->QueryInterface(mWaitContext.put());
  }
  if (const auto info = D3D11Util::DescribeColorFormat(swapchainFormat)) {
    mSwapchainTypeless = info->mTypeless;
  }
}

HostPanels::Record* HostPanels::RecordFor(uint64_t panelID) noexcept {
  Record* free = nullptr;
  for (auto& record: mRecords) {
    if (record.mPanelID == panelID) {
      return &record;
    }
    if (!free && record.mPanelID == 0) {
      free = &record;
    }
  }
  if (free) {
    free->mPanelID = panelID;
  }
  return free;
}

bool HostPanels::Bring(
  Record& record,
  const SharedAbi::PanelEntry& entry,
  const HostLink::PanelResources& resources,
  TimeUtil::TimePoint now) noexcept {
  // The host draws in our swapchain's channel order; until it does, skip
  const auto ringFormat = D3D11Util::DescribeColorFormat(static_cast<DXGI_FORMAT>(entry.mTextureFormat));
  if (!ringFormat || ringFormat->mTypeless != mSwapchainTypeless) {
    if (!record.mLoggedFormat) {
      record.mLoggedFormat = true;
      Log::Info("Panel {:X}: waiting for the host to match the swapchain format", entry.mPanelID);
    }
    return false;
  }

  if (!record.mSwapchain) {
    record.mSwapchain = std::make_unique<PanelSwapchain>(mNext, mSession, mSwapchainFormat);
  }
  auto& swapchain = *record.mSwapchain;
  const bool sizeChanged = swapchain.Width() != entry.mContentWidth || swapchain.Height() != entry.mContentHeight;
  if (!swapchain.Ensure(entry.mContentWidth, entry.mContentHeight)) {
    return false;
  }
  if (sizeChanged) {
    record.mShownGeneration = 0;// a new swapchain shows nothing yet (SY-4)
  }

  const bool current = swapchain.HasReleasedImage() && swapchain.ShownKey() == entry.mContentVersion
    && record.mShownGeneration == resources.mGeneration
    && record.mShownTextureGeneration == entry.mTextureGeneration;
  if (current) {
    record.mLateSince.reset();
    return true;// unchanged: the runtime keeps showing the last released image
  }

  const bool ready = resources.mFence->GetCompletedValue() >= entry.mFenceValue;
  bool wait = false;
  if (!ready) {
    if (swapchain.HasReleasedImage()) {
      if (!record.mLateSince) {
        record.mLateSince = now;
      }
      if (now - *record.mLateSince < mOptions.mMaxStaleness) {
        ++mCounters.mKeptPrevious;
        mStats.KeptPrevious();
        return true;// keep showing the previous image (ADR-012)
      }
    }
    // Late for too long, or nothing to show yet. Never wait on a fence whose
    // host has gone: that signal may never come
    if (!mLink.HostIsRunning()) {
      return swapchain.HasReleasedImage();
    }
    wait = true;
  }

  auto* source = resources.mTextures[entry.mCurrentSlot].get();
  if (!source) {
    return false;
  }
  {
    const auto scope = mContextState.Enter(mContext);
    if (wait) {
      // GPU-side only: the game's queue waits, its CPU never does
      mWaitContext->Wait(resources.mFence.get(), entry.mFenceValue);
      ++mCounters.mForcedWaits;
      mStats.ForcedWait();
    }
    if (!swapchain.Present(mContext, source, entry.mContentVersion)) {
      return swapchain.HasReleasedImage();
    }
  }
  record.mShownGeneration = resources.mGeneration;
  record.mShownTextureGeneration = entry.mTextureGeneration;
  record.mLateSince.reset();
  ++mCounters.mCopies;
  mStats.ImageReleased();
  return true;
}

std::size_t HostPanels::Compose(TimeUtil::TimePoint now, std::span<ComposedPanel> out) noexcept {
  if (!IsUsable()) {
    return 0;
  }
  mLink.Update(now);
  std::size_t added = 0;
  for (const auto& entry: mLink.Panels()) {
    if (added >= out.size()) {
      break;
    }
    if (!(entry.mFlags & SharedAbi::PanelFlags::Visible) || !PlausibleEntry(entry)) {
      continue;
    }
    const auto* resources = mLink.Resources(entry);
    auto* record = RecordFor(entry.mPanelID);
    if (!resources || !record || !Bring(*record, entry, *resources, now)) {
      continue;
    }
    if (!record->mSwapchain || !record->mSwapchain->HasReleasedImage()) {
      continue;
    }
    auto& quad = record->mQuad;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = mLocalSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = record->mSwapchain->Handle();
    quad.subImage.imageRect = {
      {0, 0}, {static_cast<int32_t>(record->mSwapchain->Width()), static_cast<int32_t>(record->mSwapchain->Height())}};
    quad.subImage.imageArrayIndex = 0;
    quad.pose = {
      {entry.mPose[3], entry.mPose[4], entry.mPose[5], entry.mPose[6]},
      {entry.mPose[0], entry.mPose[1], entry.mPose[2]}};
    quad.size = {entry.mSizeMeters[0], entry.mSizeMeters[1]};
    out[added++] = {&quad, 0, (entry.mFlags & SharedAbi::PanelFlags::Focused) != 0};
  }
  return added;
}

void HostPanels::Report(SharedAbi::LayerStatus& status) noexcept {
  status.mPanelsCopied = mCounters.mCopies;
  status.mKeptPrevious = mCounters.mKeptPrevious;
  status.mForcedWaits = mCounters.mForcedWaits;
  status.mPreferredPanelFormat = static_cast<uint32_t>(mSwapchainFormat);
  mLink.WriteStatus(status);
}

}// namespace OpenDesktopVR::Layer
