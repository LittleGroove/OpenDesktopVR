// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The built-in test card the layer shows before a host provides panels
// (M1 "hello quad"). Corner markers make a flipped or rotated quad obvious:
// top-left red, top-right green, bottom-left blue, bottom-right white. The
// centre is a touch target, in keeping with the product's theme.
#pragma once

#include <cstdint>
#include <vector>

namespace OpenDesktopVR::Layer {

enum class PixelOrder { BGRA, RGBA };

struct TestPatternOptions {
  uint32_t mWidth {1024};
  uint32_t mHeight {640};
  uint32_t mGridSpacing {64};
  uint32_t mCornerSize {96};
  PixelOrder mOrder {PixelOrder::BGRA};
};

// Opaque, sRGB-encoded pixels, row by row, 4 bytes each in `mOrder`
std::vector<uint32_t> MakeTestPattern(const TestPatternOptions& options);

// Colors as 0xRRGGBB, for tests and docs
namespace TestPatternColors {
inline constexpr uint32_t Background = 0x1E2430;
inline constexpr uint32_t Grid = 0x3A4458;
inline constexpr uint32_t TopLeft = 0xE04848;
inline constexpr uint32_t TopRight = 0x48C060;
inline constexpr uint32_t BottomLeft = 0x4878E0;
inline constexpr uint32_t BottomRight = 0xF0F0F0;
inline constexpr uint32_t Target = 0xF0B040;
}// namespace TestPatternColors

}// namespace OpenDesktopVR::Layer
