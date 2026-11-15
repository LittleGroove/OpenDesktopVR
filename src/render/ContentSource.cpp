// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/ContentSource.hpp>

#include <algorithm>
#include <cmath>

namespace OpenDesktopVR::Render {

Crop Crop::Clamped() const noexcept {
  const auto clamp = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 0.45f) : 0.0f; };
  return {clamp(mLeft), clamp(mTop), clamp(mRight), clamp(mBottom)};
}

}// namespace OpenDesktopVR::Render
