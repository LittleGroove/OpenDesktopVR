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

  const uint64_t completed = resources.mFence->GetCompletedValue();
  // Progress is tracked per fence: a new host or ring starts with none
  // (found by test: an old host's last value made a stuck fence look alive)
  if (record.mProgressGeneration != resources.mGeneration || record.mProgressTextureGeneration != entry.mTextureGeneration) {
    record.mProgressGeneration = resources.mGeneration;
    record.mProgressTextureGeneration = entry.mTextureGeneration;
    record.mLastCompleted = completed;
    record.mLastProgress = {};
  } else if (completed != record.mLastCompleted) {
    record.mLastCompleted = completed;
    record.mLastProgress = now;
  }
  const bool ready = completed >= entry.mFenceValue;
  bool wait = false;
  if (!ready) {
    // ADR-024 (refines ADR-012). In order:
    const bool sameRing = record.mShownGeneration == resources.mGeneration
      && record.mShownTextureGeneration == entry.mTextureGeneration;
    const bool shown = swapchain.HasReleasedImage() && sameRing;
    const uint64_t previousVersion = entry.mContentVersion - 1;

    // 1. The newest isn't done, but the one before it usually is: the host's
    //    GPU work queues behind the game's (test round 2: about half of all
    //    frames). Show it now, with no wait (doc 02: copy the newest
    //    completed slot). Only the slot just before the newest is safe: the
    //    host draws its next frame into the slot before that.
    const bool previousIsNewer = !shown || swapchain.ShownKey() < previousVersion;
    if (entry.mFenceValue >= 2 && completed >= entry.mFenceValue - 1 && previousVersion > 0 && previousIsNewer) {
      const uint32_t previousSlot = (entry.mCurrentSlot + SharedAbi::RingSlots - 1) % SharedAbi::RingSlots;
      if (auto* previous = resources.mTextures[previousSlot].get()) {
        const auto scope = mContextState.Enter(mContext);
        if (swapchain.Present(mContext, previous, previousVersion)) {
          record.mShownGeneration = resources.mGeneration;
          record.mShownTextureGeneration = entry.mTextureGeneration;
          record.mLateSince.reset();
          ++mCounters.mCopies;
          ++mCounters.mPreviousSlotCopies;
          mStats.ImageReleased();
          mStats.PreviousSlotShown();
          return true;
        }
      }
    }

    // 2. One frame behind is a normal, short lag: keep it, no timer
    if (shown && swapchain.ShownKey() + 1 >= entry.mContentVersion) {
      record.mLateSince.reset();
      ++mCounters.mKeptPrevious;
      mStats.KeptPrevious();
      return true;
    }

    // 3/4. Something of this ring is shown and we're two or more frames
    //    behind: keep it for up to 40 ms. Nothing of this ring shown yet (a
    //    new panel, or a new host whose image would replace the old one's):
    //    nothing to keep, so go straight to 5.
    if (shown) {
      if (!record.mLateSince) {
        record.mLateSince = now;
      }
      if (now - *record.mLateSince < mOptions.mMaxStaleness) {
        ++mCounters.mKeptPrevious;
        mStats.KeptPrevious();
        return true;
      }
    }

    // 5. A GPU-side wait, but only while the host is running and its fence
    //    is moving. If the host's GPU work were stuck, a wait would hold the
    //    game's GPU queue forever (a driver reset, a crashed game); then we
    //    keep what we have, or show nothing
    if (!mLink.HostIsRunning() || now - record.mLastProgress > mOptions.mProgressWindow) {
      if (!shown) {
        return false;
      }
      ++mCounters.mKeptPrevious;
      mStats.KeptPrevious();
      if (!record.mLoggedStuck) {
        record.mLoggedStuck = true;
        Log::Warning("Panel {:X}: the host's GPU work isn't completing; keeping the last image", entry.mPanelID);
      }
      return true;
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
  if (mLink.IsConnected() != mWasConnected) {
    mWasConnected = mLink.IsConnected();
    mReportDue = mWasConnected;
  }
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
