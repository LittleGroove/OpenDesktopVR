// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

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
    && entry.mContentWidth > 0 && entry.mContentHeight > 0 && entry.mTextureWidth <= 16384
    && entry.mTextureHeight <= 16384 && entry.mContentWidth <= entry.mTextureWidth
    && entry.mContentHeight <= entry.mTextureHeight && entry.mCurrentSlot < SharedAbi::RingSlots;
}

struct Extent {
  uint32_t mWidth {0};
  uint32_t mHeight {0};
};

// The size of the frame in `slot` (untrusted: within the texture, or none)
std::optional<Extent> SlotContent(const SharedAbi::PanelEntry& entry, uint32_t slot) {
  const uint32_t w = entry.mSlotContentSize[slot][0];
  const uint32_t h = entry.mSlotContentSize[slot][1];
  if (w == 0 || h == 0 || w > entry.mTextureWidth || h > entry.mTextureHeight) {
    return std::nullopt;
  }
  return Extent {w, h};
}

double MillisecondsSince(TimeUtil::TimePoint start) {
  return TimeUtil::ToMilliseconds(TimeUtil::Now() - start);
}

}// namespace

HostPanels::HostPanels(
  const NextFunctions& next,
  XrSession session,
  const XrSpace& localSpace,
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
  TimeUtil::TimePoint now,
  bool& createBudget) noexcept {
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
  // The texture's size, not the frame's: it changes only when the content
  // grows past it (RN-7)
  const bool sizeChanged = swapchain.Width() != entry.mTextureWidth || swapchain.Height() != entry.mTextureHeight;
  if (swapchain.NeedsCreate(entry.mTextureWidth, entry.mTextureHeight)) {
    if (!createBudget) {
      // One creation per frame; until then the old image (if any) stays up
      return swapchain.HasReleasedImage();
    }
    createBudget = false;
    const auto start = TimeUtil::Now();
    const bool created = swapchain.Ensure(entry.mTextureWidth, entry.mTextureHeight);
    mCosts.mSwapchainMs += MillisecondsSince(start);
    if (!created) {
      return false;
    }
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

  // For the stats: host frames between the one shown and the one about to
  // be shown were never seen in the game
  const bool sameRingShown = swapchain.HasReleasedImage() && record.mShownGeneration == resources.mGeneration
    && record.mShownTextureGeneration == entry.mTextureGeneration;
  const uint64_t shownVersion = swapchain.ShownKey();
  const auto countSkipped = [&](uint64_t version) {
    if (sameRingShown && version > shownVersion + 1) {
      mStats.HostFramesSkipped(version - shownVersion - 1);
    }
  };

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
      const auto content = SlotContent(entry, previousSlot);
      if (auto* previous = resources.mTextures[previousSlot].get(); previous && content) {
        const auto scope = mContextState.Enter(mContext);
        const auto start = TimeUtil::Now();
        const bool presented = swapchain.Present(mContext, previous, previousVersion, content->mWidth, content->mHeight);
        mCosts.mImageMs += MillisecondsSince(start);
        if (presented) {
          countSkipped(previousVersion);
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
  const auto content = SlotContent(entry, entry.mCurrentSlot).value_or(Extent {entry.mContentWidth, entry.mContentHeight});
  {
    const auto scope = mContextState.Enter(mContext);
    if (wait) {
      // GPU-side only: the game's queue waits, its CPU never does
      mWaitContext->Wait(resources.mFence.get(), entry.mFenceValue);
      ++mCounters.mForcedWaits;
      mStats.ForcedWait();
    }
    const auto start = TimeUtil::Now();
    const bool presented = swapchain.Present(mContext, source, entry.mContentVersion, content.mWidth, content.mHeight);
    mCosts.mImageMs += MillisecondsSince(start);
    if (!presented) {
      return swapchain.HasReleasedImage();
    }
  }
  countSkipped(entry.mContentVersion);
  record.mShownGeneration = resources.mGeneration;
  record.mShownTextureGeneration = entry.mTextureGeneration;
  record.mLateSince.reset();
  ++mCounters.mCopies;
  mStats.ImageReleased();
  return true;
}

std::size_t HostPanels::Compose(TimeUtil::TimePoint now, std::span<ComposedPanel> out) noexcept {
  mCosts = {};
  if (!IsUsable()) {
    return 0;
  }
  const auto linkStart = TimeUtil::Now();
  mLink.Update(now);
  mCosts.mLinkMs += MillisecondsSince(linkStart);
  if (mLink.IsConnected() != mWasConnected) {
    mWasConnected = mLink.IsConnected();
    mReportDue = mWasConnected;
  }
  std::size_t added = 0;
  // At most one shared-resource open or swapchain creation per frame: each
  // can take milliseconds (test round 4)
  bool budget = true;
  for (const auto& entry: mLink.Panels()) {
    if (added >= out.size()) {
      break;
    }
    if (!(entry.mFlags & SharedAbi::PanelFlags::Visible) || !PlausibleEntry(entry)) {
      continue;
    }
    const auto openStart = TimeUtil::Now();
    const auto* resources = mLink.Resources(entry, budget);
    mCosts.mLinkMs += MillisecondsSince(openStart);
    auto* record = RecordFor(entry.mPanelID);
    if (!resources || !record || !Bring(*record, entry, *resources, now, budget)) {
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
    // The shown frame's part of the image, and its aspect (it can be an
    // older frame than the newest, of another size)
    const auto& shown = *record->mSwapchain;
    const auto shownWidth = shown.ShownWidth();
    const auto shownHeight = shown.ShownHeight();
    quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(shownWidth), static_cast<int32_t>(shownHeight)}};
    quad.subImage.imageArrayIndex = 0;
    quad.pose = {
      {entry.mPose[3], entry.mPose[4], entry.mPose[5], entry.mPose[6]},
      {entry.mPose[0], entry.mPose[1], entry.mPose[2]}};
    const float width = entry.mSizeMeters[0];
    quad.size = {width, width * static_cast<float>(shownHeight) / static_cast<float>(shownWidth)};
    mComposedIDs[added] = entry.mPanelID;
    out[added++] = {&quad, 0, entry.mPanelID == mFocusedID};
  }
  return added;
}

bool HostPanels::UpdateFocus(const std::optional<XrPosef>& head, std::span<ComposedPanel> panels) noexcept {
  constexpr float Degrees = 3.14159265f / 180.0f;
  uint64_t focused = 0;
  if (head) {
    float nearest = 0;
    for (std::size_t i = 0; i < panels.size() && i < mComposedIDs.size(); ++i) {
      const auto& quad = *panels[i].mQuad;
      const float tolerance = (mComposedIDs[i] == mFocusedID ? 3.0f : 1.5f) * Degrees;
      if (const auto distance = XrUtil::GazeDistance(*head, quad.pose, quad.size, tolerance)) {
        if (focused == 0 || *distance < nearest) {
          focused = mComposedIDs[i];
          nearest = *distance;
        }
      }
    }
  }
  for (std::size_t i = 0; i < panels.size() && i < mComposedIDs.size(); ++i) {
    panels[i].mFocused = focused != 0 && mComposedIDs[i] == focused;
  }
  return std::exchange(mFocusedID, focused) != focused;
}

void HostPanels::Report(SharedAbi::LayerStatus& status) noexcept {
  status.mFocusedPanelID = mFocusedID;
  status.mPanelsCopied = mCounters.mCopies;
  status.mKeptPrevious = mCounters.mKeptPrevious;
  status.mForcedWaits = mCounters.mForcedWaits;
  status.mPreferredPanelFormat = static_cast<uint32_t>(mSwapchainFormat);
  mLink.WriteStatus(status);
}

}// namespace OpenDesktopVR::Layer
