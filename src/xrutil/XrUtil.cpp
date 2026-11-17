// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/XrUtil.hpp>

#include <openxr/openxr_reflection.h>

#include <algorithm>
#include <numbers>

namespace OpenDesktopVR::XrUtil {

std::string_view ResultName(XrResult result) noexcept {
  switch (result) {
#define ODVR_RESULT_CASE(name, value) \
  case name: \
    return #name;
    XR_LIST_ENUM_XrResult(ODVR_RESULT_CASE)
#undef ODVR_RESULT_CASE
    default:
      return XR_SUCCEEDED(result) ? "XR_UNKNOWN_SUCCESS" : "XR_UNKNOWN_FAILURE";
  }
}

bool IsUsable(const XrSpaceLocation& location) noexcept {
  constexpr XrSpaceLocationFlags required
    = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  return (location.locationFlags & required) == required && IsFinite(location.pose);
}

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) noexcept {
  return {
    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

XrQuaternionf Inverse(const XrQuaternionf& q) noexcept {
  const float lengthSquared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (lengthSquared <= 0) {
    return {0, 0, 0, 1};
  }
  return {-q.x / lengthSquared, -q.y / lengthSquared, -q.z / lengthSquared, q.w / lengthSquared};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) noexcept {
  const XrQuaternionf p {v.x, v.y, v.z, 0};
  const auto r = Multiply(Multiply(q, p), Inverse(q));
  return {r.x, r.y, r.z};
}

XrPosef Compose(const XrPosef& base, const XrPosef& pose) noexcept {
  const auto offset = Rotate(base.orientation, pose.position);
  return {
    Multiply(base.orientation, pose.orientation),
    {base.position.x + offset.x, base.position.y + offset.y, base.position.z + offset.z},
  };
}

XrQuaternionf FromYaw(float radians) noexcept {
  return {0, std::sin(radians / 2), 0, std::cos(radians / 2)};
}

float YawOf(const XrQuaternionf& orientation) noexcept {
  const auto forward = Rotate(orientation, {0, 0, -1});
  if (std::abs(forward.x) < 1e-4f && std::abs(forward.z) < 1e-4f) {
    return 0;
  }
  // Yaw 0 looks along -Z; positive yaw turns toward -X (counter-clockwise
  // seen from above)
  return std::atan2(-forward.x, -forward.z);
}

XrPosef LevelPose(const XrPosef& head) noexcept {
  return {FromYaw(YawOf(head.orientation)), head.position};
}

XrPosef PlaceInFront(const XrPosef& head, float forward, float drop) noexcept {
  const float yaw = YawOf(head.orientation);
  const auto orientation = FromYaw(yaw);
  const auto ahead = Rotate(orientation, {0, 0, -forward});
  return {
    orientation,
    {head.position.x + ahead.x, head.position.y - drop, head.position.z + ahead.z},
  };
}

std::optional<float> GazeDistance(
  const XrPosef& head, const XrPosef& rectPose, const XrExtent2Df& size, float toleranceRadians) noexcept {
  // The ray in the rectangle's own space, where it lies in z = 0
  const auto toLocal = Inverse(rectPose.orientation);
  const XrVector3f relative {
    head.position.x - rectPose.position.x, head.position.y - rectPose.position.y, head.position.z - rectPose.position.z};
  const auto origin = Rotate(toLocal, relative);
  const auto direction = Rotate(toLocal, Rotate(head.orientation, XrVector3f {0, 0, -1}));
  if (std::abs(direction.z) < 1e-6f) {
    return std::nullopt;// edge-on
  }
  const float t = -origin.z / direction.z;
  if (!(t > 0)) {
    return std::nullopt;
  }
  const float x = origin.x + t * direction.x;
  const float y = origin.y + t * direction.y;
  const float margin = t * std::tan(toleranceRadians);
  if (std::abs(x) > size.width / 2 + margin || std::abs(y) > size.height / 2 + margin) {
    return std::nullopt;
  }
  return t;
}

float DistanceToRect(const XrPosef& rectPose, const XrExtent2Df& size, const XrVector3f& point) noexcept {
  const XrVector3f relative {
    point.x - rectPose.position.x, point.y - rectPose.position.y, point.z - rectPose.position.z};
  const auto local = Rotate(Inverse(rectPose.orientation), relative);
  const float dx = std::max(std::abs(local.x) - size.width / 2, 0.0f);
  const float dy = std::max(std::abs(local.y) - size.height / 2, 0.0f);
  return std::sqrt(dx * dx + dy * dy + local.z * local.z);
}

}// namespace OpenDesktopVR::XrUtil
