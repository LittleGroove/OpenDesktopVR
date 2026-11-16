// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/UpdatePolicy.hpp>

#include <algorithm>
#include <chrono>

namespace OpenDesktopVR::Panels {

namespace {
// RN-5: a slightly early tick shouldn't slip a whole frame
constexpr auto Slack = std::chrono::milliseconds(4);
constexpr auto MinimumInterval = std::chrono::milliseconds(1);
}// namespace

UpdateRateLimiter::UpdateRateLimiter(double maxPerSecond) noexcept {
  SetRate(maxPerSecond, maxPerSecond);
}

void UpdateRateLimiter::SetRate(double perSecond, double globalMaximum) noexcept {
  const double global = globalMaximum > 0 ? globalMaximum : DefaultMaxUpdatesPerSecond;
  mRate = std::clamp(perSecond > 0 ? perSecond : global, 0.1, global);
  mPeriod = std::chrono::duration_cast<TimeUtil::Duration>(std::chrono::duration<double>(1.0 / mRate));
  mInterval = std::max<TimeUtil::Duration>(mPeriod - Slack, MinimumInterval);
}

bool UpdateRateLimiter::MayRender(TimeUtil::TimePoint now, bool structural) const noexcept {
  return structural || !mHasRendered || now >= mLastRender + mInterval;
}

void UpdateRateLimiter::Rendered(TimeUtil::TimePoint now) noexcept {
  mLastRender = now;
  mHasRendered = true;
}

TimeUtil::TimePoint UpdateRateLimiter::NextAllowed() const noexcept {
  return mHasRendered ? mLastRender + mPeriod : TimeUtil::TimePoint {};
}

}// namespace OpenDesktopVR::Panels
