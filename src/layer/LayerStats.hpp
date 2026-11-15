// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The layer's once-a-minute stats line (LG-2, doc 03 step 6). Counting is
// plain arithmetic on the frame thread; the line itself goes through the
// non-blocking logger. One owner (the session), no locks.
#pragma once

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace OpenDesktopVR::Layer {

class LayerStats {
 public:
  explicit LayerStats(TimeUtil::Duration period = std::chrono::seconds(60)) noexcept : mTimer(period) {
  }

  void FrameDone(double cpuMs, bool zeroLayers, uint32_t panels, uint32_t dropped) noexcept {
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
      "{}: {} frames ({} with panels, {} with no game layers); images {}; kept previous {}; "
      "forced waits {}; panels dropped {}; hand frames {} (coverage {:.1f}%); CPU avg {:.3f} ms, max {:.3f} ms",
      title,
      mFrames,
      mFramesWithPanels,
      mZeroLayerFrames,
      mImagesReleased,
      mKeptPrevious,
      mForcedWaits,
      mPanelsDropped,
      mHandFrames,
      mHandFrames ? 100.0 * mHandCoverage / static_cast<double>(mHandFrames) : 0.0,
      mCpuTotalMs / frames,
      mCpuMaxMs);
  }

 private:
  void Reset() noexcept {
    mFrames = mZeroLayerFrames = mFramesWithPanels = mPanelsDropped = 0;
    mImagesReleased = mKeptPrevious = mForcedWaits = mHandFrames = 0;
    mHandCoverage = mCpuTotalMs = mCpuMaxMs = 0;
  }

  TimeUtil::PeriodicTimer mTimer;
  uint64_t mFrames {0};
  uint64_t mZeroLayerFrames {0};
  uint64_t mFramesWithPanels {0};
  uint64_t mPanelsDropped {0};
  uint64_t mImagesReleased {0};
  uint64_t mKeptPrevious {0};
  uint64_t mForcedWaits {0};
  uint64_t mHandFrames {0};
  double mHandCoverage {0};
  double mCpuTotalMs {0};
  double mCpuMaxMs {0};
  uint64_t mTotalFrames {0};
  double mTotalCpuMs {0};
};

}// namespace OpenDesktopVR::Layer
