// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Where the hands fall in each eye's image (doc 03 "xrEndFrame work",
// step 4; XR-13): the layer draws and clears only inside this area.
// - Each joint is a sphere; its angular extent along each image axis comes
//   from the tangent lines to it, so the area is conservative.
// - A sphere reaching the eye's plane (or containing the eye) makes the
//   whole view the area; a sphere entirely behind the eye is skipped.
// - Extents past the field of view stop at the image's edge.
// Pure maths: no OpenXR calls, no allocation.
#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <optional>
#include <span>

namespace OpenDesktopVR::Layer {

struct Sphere {
  XrVector3f mCenter {};
  float mRadius {0};
};

// A pixel rectangle: [mLeft, mRight) x [mTop, mBottom)
struct PixelRect {
  int32_t mLeft {0};
  int32_t mTop {0};
  int32_t mRight {0};
  int32_t mBottom {0};

  bool IsEmpty() const noexcept {
    return mRight <= mLeft || mBottom <= mTop;
  }
  int64_t Area() const noexcept {
    return IsEmpty() ? 0 : static_cast<int64_t>(mRight - mLeft) * (mBottom - mTop);
  }
  bool operator==(const PixelRect&) const noexcept = default;
};

// The smallest rectangle holding both (an empty one adds nothing)
PixelRect Union(const PixelRect& a, const PixelRect& b) noexcept;
// `rect` grown by `pixels` on every side, then kept within `width` x `height`
PixelRect Pad(const PixelRect& rect, int32_t pixels, uint32_t width, uint32_t height) noexcept;

// The part of a `width` x `height` image, seen from `eye` with `fov`, that
// the spheres cover, grown by `marginPixels`. Empty if none is in view.
// Spheres and the eye are in the same space.
PixelRect SpheresInView(
  const XrPosef& eye,
  const XrFovf& fov,
  uint32_t width,
  uint32_t height,
  std::span<const Sphere> spheres,
  int32_t marginPixels) noexcept;

}// namespace OpenDesktopVR::Layer
