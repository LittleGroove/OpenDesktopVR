// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/ContentSource.hpp>

#include <algorithm>
#include <utility>

namespace OpenDesktopVR::Render {

namespace {

// Trims of `start` and `end` pixels from an extent of `size`: the offset and
// length left, with at least Crop::MinimumLeft (or all of a smaller extent)
std::pair<uint32_t, uint32_t> TrimExtent(uint32_t size, uint32_t start, uint32_t end) noexcept {
  start = std::min(start, Crop::MaximumEdge);
  end = std::min(end, Crop::MaximumEdge);
  const uint32_t keep = std::min(size, Crop::MinimumLeft);
  const uint64_t trimmed = static_cast<uint64_t>(start) + end;
  if (trimmed + keep <= size) {
    return {start, size - static_cast<uint32_t>(trimmed)};
  }
  // Too much: both trims shrink in proportion until `keep` is left
  const uint32_t allowed = size - keep;
  const auto scaledStart = static_cast<uint32_t>(static_cast<uint64_t>(start) * allowed / trimmed);
  return {scaledStart, keep};
}

}// namespace

PixelRect Crop::Apply(PixelSize size) const noexcept {
  const auto [left, width] = TrimExtent(size.mWidth, mLeft, mRight);
  const auto [top, height] = TrimExtent(size.mHeight, mTop, mBottom);
  return {left, top, width, height};
}

}// namespace OpenDesktopVR::Render
