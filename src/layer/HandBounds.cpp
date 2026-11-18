// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Layer/HandBounds.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

#include <algorithm>
#include <cmath>

namespace OpenDesktopVR::Layer {

namespace {

// Past this angle from straight ahead, tangents grow too fast to be
// useful: the extent runs to the image's edge
constexpr float MaxAngle = 1.48f;// about 85 degrees

struct Range {
  float mLow {0};
  float mHigh {0};
};

// The tangents (sideways / forward) that a disc of `radius` at sideways
// offset `side` and `forward` distance spans, seen from the origin. The
// disc is in front of the origin (forward > radius).
Range TangentRange(float side, float forward, float radius) {
  const float distance = std::sqrt((side * side) + (forward * forward));
  const float spread = std::asin(std::min(1.0f, radius / distance));
  const float center = std::atan2(side, forward);
  const auto tangent = [](float angle) {
    if (angle >= MaxAngle) {
      return HUGE_VALF;
    }
    if (angle <= -MaxAngle) {
      return -HUGE_VALF;
    }
    return std::tan(angle);
  };
  return {tangent(center - spread), tangent(center + spread)};
}

}// namespace

PixelRect Union(const PixelRect& a, const PixelRect& b) noexcept {
  if (a.IsEmpty()) {
    return b;
  }
  if (b.IsEmpty()) {
    return a;
  }
  return {std::min(a.mLeft, b.mLeft), std::min(a.mTop, b.mTop), std::max(a.mRight, b.mRight), std::max(a.mBottom, b.mBottom)};
}

PixelRect Pad(const PixelRect& rect, int32_t pixels, uint32_t width, uint32_t height) noexcept {
  if (rect.IsEmpty()) {
    return {};
  }
  PixelRect padded {
    std::max(0, rect.mLeft - pixels),
    std::max(0, rect.mTop - pixels),
    std::min(static_cast<int32_t>(width), rect.mRight + pixels),
    std::min(static_cast<int32_t>(height), rect.mBottom + pixels),
  };
  return padded.IsEmpty() ? PixelRect {} : padded;
}

PixelRect SpheresInView(
  const XrPosef& eye,
  const XrFovf& fov,
  uint32_t width,
  uint32_t height,
  std::span<const Sphere> spheres,
  int32_t marginPixels) noexcept {
  const PixelRect whole {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  const float tanLeft = std::tan(fov.angleLeft);
  const float tanRight = std::tan(fov.angleRight);
  const float tanUp = std::tan(fov.angleUp);
  const float tanDown = std::tan(fov.angleDown);
  if (width == 0 || height == 0 || !(tanRight > tanLeft) || !(tanUp > tanDown)) {
    return {};
  }
  const auto toEye = XrUtil::Inverse(eye.orientation);

  bool any = false;
  float lowX = HUGE_VALF;
  float highX = -HUGE_VALF;
  float lowY = HUGE_VALF;
  float highY = -HUGE_VALF;
  for (const auto& sphere: spheres) {
    if (!XrUtil::IsFinite(sphere.mCenter) || !std::isfinite(sphere.mRadius) || sphere.mRadius < 0) {
      continue;
    }
    const XrVector3f offset {
      sphere.mCenter.x - eye.position.x, sphere.mCenter.y - eye.position.y, sphere.mCenter.z - eye.position.z};
    const auto local = XrUtil::Rotate(toEye, offset);
    // The eye looks along -Z
    const float forward = -local.z;
    if (forward + sphere.mRadius <= 0) {
      continue;// entirely behind the eye
    }
    if (forward <= sphere.mRadius) {
      return Pad(whole, 0, width, height);// reaches the eye's plane: the whole view
    }
    const auto x = TangentRange(local.x, forward, sphere.mRadius);
    const auto y = TangentRange(local.y, forward, sphere.mRadius);
    lowX = std::min(lowX, x.mLow);
    highX = std::max(highX, x.mHigh);
    lowY = std::min(lowY, y.mLow);
    highY = std::max(highY, y.mHigh);
    any = true;
  }
  if (!any || highX < tanLeft || lowX > tanRight || highY < tanDown || lowY > tanUp) {
    return {};
  }
  // Tangents to pixels: x grows to the right, y grows downward
  const auto column = [&](float tangent) {
    return (std::clamp(tangent, tanLeft, tanRight) - tanLeft) / (tanRight - tanLeft) * static_cast<float>(width);
  };
  const auto row = [&](float tangent) {
    return (tanUp - std::clamp(tangent, tanDown, tanUp)) / (tanUp - tanDown) * static_cast<float>(height);
  };
  const PixelRect rect {
    static_cast<int32_t>(std::floor(column(lowX))),
    static_cast<int32_t>(std::floor(row(highY))),
    static_cast<int32_t>(std::ceil(column(highX))),
    static_cast<int32_t>(std::ceil(row(lowY))),
  };
  return Pad(rect, marginPixels, width, height);
}

}// namespace OpenDesktopVR::Layer
