// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The update-rate policy (RN-5, ADR-013) and dirty flags (RN-2, RN-3).

#include "check.hpp"

#include <OpenDesktopVR/UpdatePolicy.hpp>

#include <algorithm>
#include <chrono>
#include <format>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Panels;
using namespace std::chrono_literals;

int main() {
  using Testing::Check;
  using Testing::Scenario;

  Scenario("30/s cap: interval is 1/30 s minus 4 ms (RN-5)", [] {
    UpdateRateLimiter limiter;
    const auto ms = TimeUtil::ToMilliseconds(limiter.Interval());
    Check(ms > 29.2 && ms < 29.4, std::format("about 29.3 ms ({:.2f})", ms));
  });

  Scenario("The first change after a pause renders at once; sooner ones wait", [] {
    UpdateRateLimiter limiter;
    const TimeUtil::TimePoint t0 {std::chrono::seconds(100)};
    Check(limiter.MayRender(t0), "never rendered: render now");
    limiter.Rendered(t0);
    Check(!limiter.MayRender(t0 + 10ms), "10 ms later: postponed");
    Check(limiter.MayRender(t0 + 10ms, true), "a structural change bypasses the cap (RN-3)");
    Check(limiter.NextAllowed() == t0 + limiter.Period(), "a postponed change wakes a whole period later");
    Check(limiter.MayRender(t0 + limiter.Interval()), "but may render from 4 ms sooner (an early tick)");
    Check(limiter.MayRender(t0 + 5s), "after a pause: at once");
  });

  Scenario("A per-panel rate is clamped to the global maximum (ADR-013)", [] {
    UpdateRateLimiter limiter;
    limiter.SetRate(60, 30);
    Check(limiter.Rate() == 30, "60 asked, 30 allowed");
    limiter.SetRate(10, 30);
    Check(limiter.Rate() == 10, "a lower rate is kept");
    limiter.SetRate(0, 24);
    Check(limiter.Rate() == 24, "'same as global' follows the global maximum");
  });

  Scenario("Changes every 10 ms for 1 s: about 30 renders, and the last change is shown", [] {
    UpdateRateLimiter limiter;
    DirtyFlag dirty;
    dirty.Take();// the initial structural request
    int renders = 0;
    int lastChange = -1;
    int lastShown = -1;
    const TimeUtil::TimePoint t0 {std::chrono::seconds(1)};
    for (int ms = 0; ms <= 1100; ++ms) {
      const auto now = t0 + std::chrono::milliseconds(ms);
      if (ms % 10 == 0 && ms <= 1000) {
        lastChange = ms;
        dirty.Mark(Dirty::Content);
      }
      if (!dirty.IsSet()) {
        continue;
      }
      const auto kind = dirty.Take();
      if (!limiter.MayRender(now, kind & Dirty::Structural)) {
        dirty.Restore(kind);// postponed, never dropped
        continue;
      }
      limiter.Rendered(now);
      lastShown = lastChange;
      ++renders;
    }
    Check(renders >= 30 && renders <= 36, std::format("{} renders in 1.1 s", renders));
    Check(lastShown == 1000, std::format("the final change (t=1000 ms) was rendered (last shown {})", lastShown));
  });

  Scenario("An animated panel woken by a precise timer renders at its cap, not above (test round 5)", [] {
    UpdateRateLimiter limiter;
    const TimeUtil::TimePoint t0 {std::chrono::seconds(10)};
    auto now = t0;
    int renders = 0;
    while (now < t0 + 2s) {
      if (limiter.MayRender(now)) {
        limiter.Rendered(now);
        ++renders;
      }
      // The timer fires when asked, plus a little wake-up latency
      now = std::max(limiter.NextAllowed(), now) + 300us;
    }
    Check(renders >= 58 && renders <= 61, std::format("{} renders in 2 s at a cap of 30/s", renders));
  });

  Scenario("A dirty flag is taken before drawing, so mid-render changes survive (RN-2)", [] {
    DirtyFlag dirty;
    Check(dirty.Take() == Dirty::Structural, "a new panel starts with a structural request");
    dirty.Mark(Dirty::Content);
    const auto taken = dirty.Take();
    dirty.Mark(Dirty::Content);// arrives while "drawing"
    Check(taken == Dirty::Content, "the render took the content request");
    Check(dirty.IsSet(), "the change made mid-render is still pending");
  });

  return Testing::Finish();
}
