// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// When a panel may render (doc 08 "Update-rate policy", ADR-013):
// - content-driven: only dirty panels render (RN-1);
// - at most `rate` times a second, with the interval 1/rate minus 4 ms so a
//   slightly early tick doesn't slip a whole frame (RN-5);
// - the first change after a pause renders at once;
// - changes that come sooner are postponed, never dropped: the caller keeps
//   the panel dirty and wakes at NextAllowed();
// - structural changes bypass the cap (RN-3).
//
// Dirty flags are taken atomically before drawing, never cleared after
// (RN-2), so a change made mid-render is never lost.
#pragma once

#include <OpenDesktopVR/TimeUtil.hpp>

#include <atomic>
#include <cstdint>

namespace OpenDesktopVR::Panels {

inline constexpr double DefaultMaxUpdatesPerSecond = 30.0;

class UpdateRateLimiter {
 public:
  explicit UpdateRateLimiter(double maxPerSecond = DefaultMaxUpdatesPerSecond) noexcept;

  // Per-panel rate, clamped to the global maximum (ADR-013)
  void SetRate(double perSecond, double globalMaximum = DefaultMaxUpdatesPerSecond) noexcept;
  double Rate() const noexcept {
    return mRate;
  }
  TimeUtil::Duration Interval() const noexcept {
    return mInterval;
  }

  bool MayRender(TimeUtil::TimePoint now, bool structural = false) const noexcept;
  void Rendered(TimeUtil::TimePoint now) noexcept;
  // The earliest time a postponed change may render
  TimeUtil::TimePoint NextAllowed() const noexcept;

 private:
  double mRate {DefaultMaxUpdatesPerSecond};
  TimeUtil::Duration mInterval {};
  TimeUtil::TimePoint mLastRender {};
  bool mHasRendered {false};
};

namespace Dirty {
inline constexpr uint32_t None = 0;
inline constexpr uint32_t Content = 1u << 0;   // this panel's pixels changed
inline constexpr uint32_t Structural = 1u << 1;// size, settings, list changes
}// namespace Dirty

// Marked from any thread; taken by the render thread before it draws
class DirtyFlag {
 public:
  void Mark(uint32_t kind = Dirty::Content) noexcept {
    mBits.fetch_or(kind, std::memory_order_acq_rel);
  }
  // Takes and clears the request (RN-2)
  uint32_t Take() noexcept {
    return mBits.exchange(Dirty::None, std::memory_order_acq_rel);
  }
  // Puts back a request that couldn't render yet (postponed by the cap)
  void Restore(uint32_t kind) noexcept {
    Mark(kind);
  }
  bool IsSet() const noexcept {
    return mBits.load(std::memory_order_acquire) != Dirty::None;
  }

 private:
  std::atomic<uint32_t> mBits {Dirty::Structural};// new panels need a first render
};

}// namespace OpenDesktopVR::Panels
