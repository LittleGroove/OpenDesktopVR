// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The product's one palette (ADR-023: one theme, "use your hands"). Every
// surface uses these: panels drawn by the host, the settings window, the
// mirror, menus in VR. Colours are 0xRRGGBB, sRGB-encoded.
#pragma once

#include <cstdint>

namespace OpenDesktopVR::Theme {

inline constexpr uint32_t Background = 0x1E2430;// dark slate
inline constexpr uint32_t Surface = 0x262E3C;   // raised areas, fields
inline constexpr uint32_t SurfaceHigh = 0x323C4E;
inline constexpr uint32_t Line = 0x3A4458;      // grid lines, borders
inline constexpr uint32_t Text = 0xF0F0F0;
inline constexpr uint32_t TextDim = 0xA8B0C0;
inline constexpr uint32_t Accent = 0xF0B040;    // touch targets, the selected item
inline constexpr uint32_t AccentDim = 0xB08530;
// Status marks (the Diagnostics tab's checks)
inline constexpr uint32_t StatusGood = 0x48C060;   // green
inline constexpr uint32_t StatusWarning = 0xE8A838;// amber
inline constexpr uint32_t StatusProblem = 0xE04848;// red

// Orientation markers on test cards and patterns
inline constexpr uint32_t CornerTopLeft = 0xE04848;// red
inline constexpr uint32_t CornerTopRight = 0x48C060;// green
inline constexpr uint32_t CornerBottomLeft = 0x4878E0;// blue
inline constexpr uint32_t CornerBottomRight = 0xF0F0F0;// white

struct Rgba {
  float r, g, b, a;
};

constexpr Rgba ToRgba(uint32_t rgb, float alpha = 1.0f) noexcept {
  return {
    static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
    static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
    static_cast<float>(rgb & 0xFF) / 255.0f,
    alpha,
  };
}

}// namespace OpenDesktopVR::Theme
