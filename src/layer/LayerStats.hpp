// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The layer's once-a-minute stats line (LG-2, doc 03 step 6). Counting is
// plain arithmetic on the frame thread; the line itself goes through the
// non-blocking logger. One owner (the session), no locks.
//
// Doc 08's budget is 0.2 ms typical and 0.5 ms at the 99th percentile: the
// line counts frames over 0.5 ms and says where the slowest one's time went
// (test round 5 saw single frames of 22-27 ms with no way to tell why).
#pragma once

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace OpenDesktopVR::Layer {

// Where one frame's time went, in the parts that call out of the layer
struct FrameCosts {
  double mLinkMs {0};     // the host link: connecting, opening shared textures
  double mSwapchainMs {0};// creating panel swapchains
  double mImageMs {0};    // acquiring, waiting for, copying and releasing images
  double mHandsMs {0};    // tracking and drawing hands
};

class LayerStats {
 public:
  static constexpr double SlowFrameMs = 0.5;

  explicit LayerStats(TimeUtil::Duration period = std::chrono::seconds(60)) noexcept : mTimer(period) {
  }

  void FrameDone(double cpuMs, bool zeroLayers, uint32_t panels, uint32_t dropped, const FrameCosts& costs = {}) noexcept {
    if (cpuMs > SlowFrameMs) {
      ++mSlowFrames;
    }
    if (cpuMs > mCpuMaxMs) {
      mSlowest = costs;
    }
    ++mFrames;
    mZeroLayerFrames += zeroLayers ? 1 : 0;
    mFramesWithPanels += panels > 0 ? 1 : 0;
    mPanelsDropped += dropped;
    mCpuTotalMs += cpuMs;
    mCpuMaxMs = std::max(mCpuMaxMs, cpuMs);
    ++mTotalFrames;
    mTotalCpuMs += cpuMs;
  }
  void ImageReleased() noexcept {
    ++mImagesReleased;
  }
  void KeptPrevious() noexcept {
    ++mKeptPrevious;
  }
  void ForcedWait() noexcept {
    ++mForcedWaits;
  }
  void PreviousSlotShown() noexcept {
    ++mPreviousSlot;
  }
  // Frames the host drew that the game never showed (a newer one came
  // first): the frames a video loses on the way (test round 7)
  void HostFramesSkipped(uint64_t count) noexcept {
    mHostFramesSkipped += count;
  }
  void HandFrame(double coverage) noexcept {
    ++mHandFrames;
    mHandCoverage += coverage;
  }

  // Writes the line if a period has passed; at most one line per frame
  void MaybeLog(TimeUtil::TimePoint now = TimeUtil::Now()) noexcept {
    if (mTimer.Due(now)) {
      Write("Layer stats (60 s)");
      Reset();
    }
  }

  // The whole session, for the line written at session end
  double AverageCpuMs() const noexcept {
    return mTotalFrames ? mTotalCpuMs / static_cast<double>(mTotalFrames) : 0;
  }
  uint64_t TotalFrames() const noexcept {
    return mTotalFrames;
  }

  void Write(const char* title) const noexcept {
    const double frames = static_cast<double>(std::max<uint64_t>(mFrames, 1));
    Log::Info(
      "{}: {} frames ({} with panels, {} with no game layers); images {} ({} from the slot before the newest; "
      "{} host frames never shown); kept previous {}; forced waits {}; panels dropped {}; hand frames {} (coverage {:.1f}%); "
      "CPU avg {:.3f} ms, max {:.3f} ms (host link {:.3f}, swapchain creation {:.3f}, images {:.3f}, hands {:.3f}); "
      "frames over {} ms: {}",
      title,
      mFrames,
      mFramesWithPanels,
      mZeroLayerFrames,
      mImagesReleased,
      mPreviousSlot,
      mHostFramesSkipped,
      mKeptPrevious,
      mForcedWaits,
      mPanelsDropped,
      mHandFrames,
      mHandFrames ? 100.0 * mHandCoverage / static_cast<double>(mHandFrames) : 0.0,
      mCpuTotalMs / frames,
      mCpuMaxMs,
      mSlowest.mLinkMs,
      mSlowest.mSwapchainMs,
      mSlowest.mImageMs,
      mSlowest.mHandsMs,
      SlowFrameMs,
      mSlowFrames);
  }

 private:
  void Reset() noexcept {
    mFrames = mZeroLayerFrames = mFramesWithPanels = mPanelsDropped = 0;
    mImagesReleased = mKeptPrevious = mForcedWaits = mHandFrames = mPreviousSlot = mHostFramesSkipped = 0;
    mHandCoverage = mCpuTotalMs = mCpuMaxMs = 0;
    mSlowFrames = 0;
    mSlowest = {};
  }

  TimeUtil::PeriodicTimer mTimer;
  uint64_t mFrames {0};
  uint64_t mZeroLayerFrames {0};
  uint64_t mFramesWithPanels {0};
  uint64_t mPanelsDropped {0};
  uint64_t mImagesReleased {0};
  uint64_t mKeptPrevious {0};
  uint64_t mForcedWaits {0};
  uint64_t mPreviousSlot {0};
  uint64_t mHostFramesSkipped {0};
  uint64_t mHandFrames {0};
  double mHandCoverage {0};
  double mCpuTotalMs {0};
  double mCpuMaxMs {0};
  uint64_t mSlowFrames {0};
  FrameCosts mSlowest;// the parts of the frame that took mCpuMaxMs
  uint64_t mTotalFrames {0};
  double mTotalCpuMs {0};
};

}// namespace OpenDesktopVR::Layer
