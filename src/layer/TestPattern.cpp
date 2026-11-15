// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Layer/TestPattern.hpp>

#include <algorithm>
#include <cmath>

namespace OpenDesktopVR::Layer {

namespace {

uint32_t Pack(uint32_t rgb, PixelOrder order) {
  const uint32_t r = (rgb >> 16) & 0xFF;
  const uint32_t g = (rgb >> 8) & 0xFF;
  const uint32_t b = rgb & 0xFF;
  // Little-endian: the first byte in memory is the lowest
  return order == PixelOrder::BGRA ? (0xFFu << 24) | (r << 16) | (g << 8) | b
                                   : (0xFFu << 24) | (b << 16) | (g << 8) | r;
}

}// namespace

std::vector<uint32_t> MakeTestPattern(const TestPatternOptions& options) {
  using namespace TestPatternColors;
  const uint32_t w = options.mWidth;
  const uint32_t h = options.mHeight;
  std::vector<uint32_t> pixels(static_cast<std::size_t>(w) * h, Pack(Background, options.mOrder));
  const float cx = (static_cast<float>(w) - 1) / 2;
  const float cy = (static_cast<float>(h) - 1) / 2;
  const float unit = static_cast<float>(std::min(w, h)) / 16;

  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t color = Background;
      const bool gridLine = options.mGridSpacing > 0
        && (x % options.mGridSpacing < 2 || y % options.mGridSpacing < 2);
      if (gridLine || x < 4 || y < 4 || x >= w - 4 || y >= h - 4) {
        color = Grid;
      }
      const bool left = x < options.mCornerSize;
      const bool right = x >= w - options.mCornerSize;
      const bool top = y < options.mCornerSize;
      const bool bottom = y >= h - options.mCornerSize;
      if (top && left) {
        color = TopLeft;
      } else if (top && right) {
        color = TopRight;
      } else if (bottom && left) {
        color = BottomLeft;
      } else if (bottom && right) {
        color = BottomRight;
      }
      // Touch target: a dot and two rings
      const float d = std::hypot(static_cast<float>(x) - cx, static_cast<float>(y) - cy);
      if (d < unit * 0.6f || std::abs(d - unit * 1.6f) < unit * 0.18f || std::abs(d - unit * 2.8f) < unit * 0.12f) {
        color = Target;
      }
      pixels[static_cast<std::size_t>(y) * w + x] = Pack(color, options.mOrder);
    }
  }
  return pixels;
}

}// namespace OpenDesktopVR::Layer
