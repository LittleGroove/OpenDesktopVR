// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Synthetic hands for headset-free tests.
#pragma once

#include <OpenDesktopVR/HandInteraction.hpp>

#include <array>
#include <cmath>
#include <numbers>

namespace OpenDesktopVR::SyntheticHands {

using namespace OpenDesktopVR::HandInteraction;

enum class Pose {
  Point,
  PointRelaxedLittle,
  // Index and middle fingers out
  TwoFingers,
  Flat,
  Fist,
  Relaxed,
};

struct FingerAngles {
  float mMCP {};
  float mPIP {};
  float mDIP {};
};

// Index, middle, ring, little; degrees of bend toward the palm
inline std::array<FingerAngles, 4> AnglesFor(Pose pose) {
  constexpr FingerAngles straight {5, 5, 5};
  constexpr FingerAngles curled {80, 100, 60};
  constexpr FingerAngles fist {85, 100, 70};
  constexpr FingerAngles relaxed {20, 30, 15};
  switch (pose) {
    case Pose::Point:
      return {straight, curled, curled, curled};
    case Pose::PointRelaxedLittle:
      return {straight, curled, curled, FingerAngles {30, 40, 20}};
    case Pose::TwoFingers:
      return {straight, straight, curled, curled};
    case Pose::Flat:
      return {straight, straight, straight, straight};
    case Pose::Fist:
      return {fist, fist, fist, fist};
    case Pose::Relaxed:
      return {relaxed, relaxed, relaxed, relaxed};
  }
  return {};
}

// World-space directions of the hand's axes: +X across the knuckles (toward
// the thumb), +Y from the wrist toward the fingers, +Z out of the back of the
// hand
struct HandBasis {
  Vector3 mAcross;
  Vector3 mForward;
  Vector3 mBack;
};

// Right hand, fingers pointing forward (-Z), palm down
inline const HandBasis PokeBasis {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
// Right hand, fingers pointing left (-X), palm down
inline const HandBasis SidewaysBasis {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}};
// Left hand, fingers pointing forward (-Z), palm down
inline const HandBasis LeftPokeBasis {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};

// Builds a hand with its index fingertip at `indexTipTarget`
inline HandJoints
MakeHand(Pose pose, const HandBasis& basis, const Vector3& indexTipTarget) {
  std::array<Vector3, JointCount> local {};
  std::array<float, JointCount> radii {};
  const auto set = [&](Joint j, const Vector3& v, float radius) {
    local[static_cast<std::size_t>(j)] = v;
    radii[static_cast<std::size_t>(j)] = radius;
  };
  set(Joint::Wrist, {0, 0, 0}, 0.018f);
  set(Joint::Palm, {0, 0.05f, 0}, 0.02f);
  set(Joint::ThumbMetacarpal, {0.025f, 0.02f, -0.01f}, 0.016f);
  set(Joint::ThumbProximal, {0.045f, 0.045f, -0.015f}, 0.0115f);
  set(Joint::ThumbDistal, {0.06f, 0.07f, -0.015f}, 0.0095f);
  set(Joint::ThumbTip, {0.07f, 0.09f, -0.015f}, 0.0085f);

  struct FingerLayout {
    Joint mMetacarpal;
    Vector3 mMetacarpalPosition;
    Vector3 mKnuckle;
    std::array<float, 3> mLengths;
    float mScale;
  };
  const std::array<FingerLayout, 4> fingers {{
    {Joint::IndexMetacarpal,
     {0.02f, 0.01f, 0},
     {0.025f, 0.085f, 0},
     {0.040f, 0.024f, 0.020f},
     1.0f},
    {Joint::MiddleMetacarpal,
     {0.005f, 0.01f, 0},
     {0.005f, 0.088f, 0},
     {0.045f, 0.028f, 0.021f},
     1.03f},
    {Joint::RingMetacarpal,
     {-0.01f, 0.01f, 0},
     {-0.015f, 0.083f, 0},
     {0.042f, 0.027f, 0.021f},
     0.97f},
    {Joint::LittleMetacarpal,
     {-0.022f, 0.005f, 0},
     {-0.032f, 0.073f, 0},
     {0.033f, 0.019f, 0.019f},
     0.85f},
  }};

  const auto angles = AnglesFor(pose);
  // Curling rotates the finger from +Y toward the palm (-Z)
  const auto direction = [](float degrees) {
    const float radians = degrees * std::numbers::pi_v<float> / 180;
    return Vector3 {0, std::cos(radians), -std::sin(radians)};
  };
  for (std::size_t f = 0; f < fingers.size(); ++f) {
    const auto& finger = fingers[f];
    const auto& a = angles[f];
    const auto base = static_cast<std::size_t>(finger.mMetacarpal);
    const Vector3 proximal = finger.mKnuckle;
    const Vector3 intermediate
      = proximal + direction(a.mMCP) * finger.mLengths[0];
    const Vector3 distal
      = intermediate + direction(a.mMCP + a.mPIP) * finger.mLengths[1];
    const Vector3 tip
      = distal + direction(a.mMCP + a.mPIP + a.mDIP) * finger.mLengths[2];
    local[base + 0] = finger.mMetacarpalPosition;
    local[base + 1] = proximal;
    local[base + 2] = intermediate;
    local[base + 3] = distal;
    local[base + 4] = tip;
    radii[base + 0] = 0.011f * finger.mScale;
    radii[base + 1] = 0.0105f * finger.mScale;
    radii[base + 2] = 0.009f * finger.mScale;
    radii[base + 3] = 0.008f * finger.mScale;
    radii[base + 4] = 0.0075f * finger.mScale;
  }

  const auto toWorld = [&](const Vector3& p) {
    return (basis.mAcross * p.x) + (basis.mForward * p.y)
      + (basis.mBack * p.z);
  };
  const Vector3 offset = indexTipTarget
    - toWorld(local[static_cast<std::size_t>(Joint::IndexTip)]);

  HandJoints hand {.mIsTracked = true};
  for (std::size_t i = 0; i < JointCount; ++i) {
    hand.mJoints[i].mPosition = toWorld(local[i]) + offset;
    hand.mJoints[i].mOrientation = Quaternion::Identity;
    hand.mJoints[i].mRadius = radii[i];
  }
  return hand;
}

}// namespace OpenDesktopVR::SyntheticHands
