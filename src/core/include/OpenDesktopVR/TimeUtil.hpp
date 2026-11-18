// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Time helpers: one monotonic clock for the whole product, and small
// rate-limiting tools. Header-only, allocation-free, safe in the game
// process.
#pragma once

#include <atomic>
#include <chrono>

namespace OpenDesktopVR::TimeUtil {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using TimePoint = Clock::time_point;

inline TimePoint Now() noexcept {
  return Clock::now();
}

template <class Rep, class Period>
constexpr double ToMilliseconds(std::chrono::duration<Rep, Period> d) noexcept {
  return std::chrono::duration<double, std::milli>(d).count();
}

// Lets an action through at most once per interval; thread-safe and
// lock-free. Used for per-hand log detail (every 1 s near a panel, every
// 10 s otherwise; LG-1) and similar "don't spam" cases.
class RateLimiter {
 public:
  explicit constexpr RateLimiter(Duration interval) noexcept : mInterval(interval) {
  }

  // True if the action may run now; if so, the next one waits an interval
  bool Allow(TimePoint now = Now()) noexcept {
    auto next = mNextAllowed.load(std::memory_order_relaxed);
    const auto nowTicks = now.time_since_epoch().count();
    while (nowTicks >= next) {
      if (mNextAllowed.compare_exchange_weak(next, nowTicks + mInterval.count(), std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  void SetInterval(Duration interval) noexcept {
    mInterval = interval;
  }

  void Reset() noexcept {
    mNextAllowed.store(0, std::memory_order_relaxed);
  }

 private:
  Duration mInterval;
  std::atomic<Duration::rep> mNextAllowed {0};
};

// Fires once per period, measured from the first call (stats lines every
// 60 s, LG-2). Not thread-safe: one owner.
class PeriodicTimer {
 public:
  explicit constexpr PeriodicTimer(Duration period) noexcept : mPeriod(period) {
  }

  bool Due(TimePoint now = Now()) noexcept {
    if (mNext == TimePoint {}) {
      mNext = now + mPeriod;
      return false;
    }
    if (now < mNext) {
      return false;
    }
    // Skip missed periods rather than firing several times in a row
    while (mNext <= now) {
      mNext += mPeriod;
    }
    return true;
  }

 private:
  Duration mPeriod;
  TimePoint mNext {};
};

}// namespace OpenDesktopVR::TimeUtil
