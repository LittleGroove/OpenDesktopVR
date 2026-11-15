// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// OpenXR helpers shared by the layer, the host and tools: result names,
// number validation, struct-chain search and pose maths. Header-light: only
// <openxr/openxr.h>.
#pragma once

#include <openxr/openxr.h>

#include <cmath>
#include <string_view>

namespace OpenDesktopVR::XrUtil {

// Our own name table (built from the public reflection header); the
// loader's result-to-string function fails on some runtimes
std::string_view ResultName(XrResult result) noexcept;

// ---- Validation (XR-10) ----------------------------------------------------

inline bool IsFinite(float v) noexcept {
  return std::isfinite(v);
}
inline bool IsFinite(const XrVector3f& v) noexcept {
  return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z);
}
inline bool IsFinite(const XrQuaternionf& q) noexcept {
  return IsFinite(q.x) && IsFinite(q.y) && IsFinite(q.z) && IsFinite(q.w);
}
inline bool IsFinite(const XrPosef& pose) noexcept {
  return IsFinite(pose.position) && IsFinite(pose.orientation);
}

// Both position and orientation valid, and every number finite
bool IsUsable(const XrSpaceLocation& location) noexcept;

// ---- Struct chains -----------------------------------------------------------

// The first struct of `type` in a `next` chain, or null
template <class T>
const T* FindInChain(const void* next, XrStructureType type) noexcept {
  for (auto it = static_cast<const XrBaseInStructure*>(next); it; it = it->next) {
    if (it->type == type) {
      return reinterpret_cast<const T*>(it);
    }
  }
  return nullptr;
}

template <class T>
T* FindInChain(void* next, XrStructureType type) noexcept {
  for (auto it = static_cast<XrBaseOutStructure*>(next); it; it = it->next) {
    if (it->type == type) {
      return reinterpret_cast<T*>(it);
    }
  }
  return nullptr;
}

// ---- Pose maths --------------------------------------------------------------

inline constexpr XrPosef IdentityPose {{0, 0, 0, 1}, {0, 0, 0}};

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) noexcept;
XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) noexcept;
XrQuaternionf Inverse(const XrQuaternionf& q) noexcept;
// `pose` expressed in a space whose origin is `base`: base * pose
XrPosef Compose(const XrPosef& base, const XrPosef& pose) noexcept;
XrQuaternionf FromYaw(float radians) noexcept;

// The yaw (rotation about +Y) of the direction a pose looks along (-Z).
// Looking straight up or down falls back to 0.
float YawOf(const XrQuaternionf& orientation) noexcept;

// A level pose `forward` meters in front of `head` (yaw only, so panels stay
// level; doc 03 "Recenter"), `drop` meters below it, facing the head
XrPosef PlaceInFront(const XrPosef& head, float forward, float drop) noexcept;

// Distance from `point` to the closest point of a rectangle centred on
// `rectPose` (in its XY plane), for back-to-front panel order
float DistanceToRect(const XrPosef& rectPose, const XrExtent2Df& size, const XrVector3f& point) noexcept;

}// namespace OpenDesktopVR::XrUtil
