// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Headset-free tests for the 'point, then tap to click' logic, using synthetic
// hands. Run it from a terminal; it exits with 0 if every scenario passes.

#include "synthetic-hands.hpp"

#include <OpenDesktopVR/HandInteraction.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace OpenDesktopVR::HandInteraction;
using namespace OpenDesktopVR::SyntheticHands;

namespace {

constexpr double FrameTime = 1.0 / 90;

const Vector3 Viewer {0, 1.2f, 0};
// Panel A is 30cm wide
constexpr float metersPerU = 0.30f;

const Panel PanelA {
  .mID = 0xA,
  .mPosition = {0, 1.0f, -0.5f},
  .mOrientation = Quaternion::Identity,
  .mSize = {0.30f, 0.20f},
};

const Panel PanelB {
  .mID = 0xB,
  .mPosition = {0.4f, 1.0f, -0.5f},
  .mOrientation = Quaternion::Identity,
  .mSize = {0.30f, 0.20f},
};


struct Recorder {
  std::vector<CursorUpdate> mUpdates;
  int mTouches {0};
  int mReleases {0};
  bool mHoveredBeforeFirstTouch {false};
  std::optional<CursorUpdate> mFirstTouch;
  std::optional<CursorUpdate> mLastTouch;
  std::vector<CursorUpdate> mTouchDowns;
  // The update that ended the first tap, and how far in front of the panel
  // the real fingertip was at that moment
  std::optional<CursorUpdate> mFirstRelease;
  std::optional<float> mFirstReleaseTipDistance;
  std::optional<CursorUpdate> mLastHover;
  // Scrolling
  int32_t mWheelTotal {0};
  std::vector<CursorUpdate> mWheelUpdates;
  TouchState mLastState {TouchState::NotNear};
  float mLastVisibility {0};
  // Where the real (unsmoothed) fingertip is
  PanelProjection mTruth {};

  void Record(const FrameResult& result, const PanelProjection& truth) {
    mTruth = truth;
    mLastVisibility = result.mHandVisibility[1];
    for (const auto& update: result.mCursorUpdates) {
      mUpdates.push_back(update);
      if (update.mWheel != 0) {
        mWheelTotal += update.mWheel;
        mWheelUpdates.push_back(update);
      }
      if (update.mState == TouchState::Hovering) {
        mLastHover = update;
        if (!mFirstTouch) {
          mHoveredBeforeFirstTouch = true;
        }
      }
      if (update.mState == TouchState::Touching) {
        if (mLastState != TouchState::Touching) {
          ++mTouches;
          mTouchDowns.push_back(update);
          if (!mFirstTouch) {
            mFirstTouch = update;
          }
        }
        mLastTouch = update;
      } else if (mLastState == TouchState::Touching) {
        ++mReleases;
        if (!mFirstRelease) {
          mFirstRelease = update;
          mFirstReleaseTipDistance = truth.mDistance;
        }
      }
      mLastState = update.mState;
    }
  }
};

class Simulation {
 public:
  explicit Simulation(
    std::vector<Panel> panels = {PanelA},
    const Settings& settings = {},
    uint32_t seed = 12345)
    : mTracker(settings),
      mPanels(std::move(panels)),
      mRandom(seed) {
  }

  // Holds the right hand still
  void Hold(Pose pose, const HandBasis& basis, Vector3 tip, double seconds) {
    Move(pose, basis, tip, tip, seconds);
  }

  // Moves the right hand's index fingertip in a straight line
  void Move(
    Pose pose,
    const HandBasis& basis,
    Vector3 from,
    Vector3 to,
    double seconds,
    float noise = 0) {
    const int frames = std::max(1, static_cast<int>(seconds / FrameTime));
    for (int i = 0; i < frames; ++i) {
      const float t = static_cast<float>(i + 1) / frames;
      Vector3 tip = Vector3::Lerp(from, to, t);
      if (noise > 0) {
        tip += Vector3 {mNoise(mRandom), mNoise(mRandom), mNoise(mRandom)}
          * noise;
      }
      Step(MakeHandAtContact(pose, basis, tip));
    }
  }

  // Like MakeHand(), but for two fingers, places the point between the
  // fingertips (where scrolling touches) at `contact`
  static HandJoints
  MakeHandAtContact(Pose pose, const HandBasis& basis, const Vector3& contact) {
    auto hand = MakeHand(pose, basis, contact);
    if (pose != Pose::TwoFingers) {
      return hand;
    }
    const auto midpoint
      = (hand[Joint::IndexTip].mPosition + hand[Joint::MiddleTip].mPosition)
      * 0.5f;
    const auto offset = contact - midpoint;
    for (auto& joint: hand.mJoints) {
      joint.mPosition += offset;
    }
    return hand;
  }

  void LoseTracking(double seconds) {
    const int frames = std::max(1, static_cast<int>(seconds / FrameTime));
    for (int i = 0; i < frames; ++i) {
      Step(HandJoints {});
    }
  }

  void Step(const HandJoints& right) {
    // Shares StepBoth(); unchanged behavior
    StepBoth(HandJoints {}, right);
  }

  // Both hands. The recorder's 'truth'
  // is still the right hand.
  void StepBoth(const HandJoints& left, const HandJoints& right) {
    const auto result = mTracker.Update(mNow, {left, right}, mPanels, Viewer);
    mRecorder.Record(
      result,
      right.mIsTracked ? ProjectOntoPanel(
                           right[Joint::IndexTip].mPosition, mPanels.front(), Viewer)
                       : mRecorder.mTruth);
    mNow += FrameTime;
  }

  // Moves both index fingertips in
  // straight lines, at the same time
  void MoveBoth(
    Pose pose,
    Vector3 leftFrom,
    Vector3 leftTo,
    Vector3 rightFrom,
    Vector3 rightTo,
    double seconds) {
    const int frames = std::max(1, static_cast<int>(seconds / FrameTime));
    for (int i = 0; i < frames; ++i) {
      const float t = static_cast<float>(i + 1) / frames;
      StepBoth(
        MakeHandAtContact(pose, LeftPokeBasis, Vector3::Lerp(leftFrom, leftTo, t)),
        MakeHandAtContact(pose, PokeBasis, Vector3::Lerp(rightFrom, rightTo, t)));
    }
  }

  Recorder mRecorder;

 private:
  Tracker mTracker;
  std::vector<Panel> mPanels;
  double mNow {1000};
  std::mt19937 mRandom {12345};
  std::uniform_real_distribution<float> mNoise {-1.0f, 1.0f};
};

// Panel A is at z = -0.5; the viewer is in front of it (+Z)
Vector3 OnA(float x, float y, float distanceInFront) {
  return {x, 1.0f + y, -0.5f + distanceInFront};
}

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what) {
  ++gChecks;
  if (condition) {
    std::printf("    ok:   %s\n", what.c_str());
    return;
  }
  ++gFailures;
  std::printf("    FAIL: %s\n", what.c_str());
}

void Scenario(const char* name, const std::function<void()>& body) {
  std::printf("%s\n", name);
  body();
}

}// namespace

int main() {
  Scenario("Finger curls are measured correctly", [] {
    const auto point = GetFingerCurls(MakeHand(Pose::Point, PokeBasis, {}));
    Check(point.mIndex < 0.2f, std::format("pointing index is straight ({:.2f})", point.mIndex));
    Check(point.mMiddle > 0.9f, std::format("pointing middle is curled ({:.2f})", point.mMiddle));
    const auto flat = GetFingerCurls(MakeHand(Pose::Flat, PokeBasis, {}));
    Check(flat.mMiddle < 0.2f, std::format("flat middle is straight ({:.2f})", flat.mMiddle));
    const auto fist = GetFingerCurls(MakeHand(Pose::Fist, PokeBasis, {}));
    Check(fist.mIndex > 0.9f, std::format("fist index is curled ({:.2f})", fist.mIndex));
  });

  Scenario("Panel projection", [] {
    const auto p = ProjectOntoPanel(OnA(0.15f, 0.10f, 0.05f), PanelA, Viewer);
    Check(std::abs(p.mU - 1.0f) < 1e-4f && std::abs(p.mV) < 1e-4f, "top-right corner maps to (1, 0)");
    Check(std::abs(p.mDistance - 0.05f) < 1e-4f, "distance in front is positive");
    Check(p.mEdgeDistance < 1e-4f, "corner is on the panel");
    const auto outside = ProjectOntoPanel(OnA(0.25f, 0, 0), PanelA, Viewer);
    Check(std::abs(outside.mEdgeDistance - 0.10f) < 1e-4f, "10cm past the edge is reported");
    const auto fromBehind
      = ProjectOntoPanel(OnA(0, 0, -0.05f), PanelA, {0, 1.2f, -1.0f});
    Check(std::abs(fromBehind.mDistance - 0.05f) < 1e-4f, "'front' follows the viewer's side");

    Panel rotated = PanelA;
    rotated.mOrientation = Quaternion::CreateFromAxisAngle(
      Vector3::UnitX, -std::numbers::pi_v<float> / 4);
    const auto normal = Vector3::Transform(Vector3::UnitZ, rotated.mOrientation);
    const auto r = ProjectOntoPanel(rotated.mPosition + normal * 0.03f, rotated, Viewer);
    Check(std::abs(r.mDistance - 0.03f) < 1e-4f && std::abs(r.mU - 0.5f) < 1e-4f && std::abs(r.mV - 0.5f) < 1e-4f, "tilted panel: 3cm along its normal");
  });

  // Drawing panels back to front, and
  // drawing hands so they don't poke through panels
  Scenario("Distance to a panel", [] {
    Check(std::abs(DistanceToPanel(OnA(0, 0, 0.2f), PanelA) - 0.2f) < 1e-4f, "straight in front: the distance to the surface");
    Check(std::abs(DistanceToPanel(OnA(0.25f, 0, 0.2f), PanelA) - std::sqrt(0.05f)) < 1e-4f, "beside it: the distance to the nearest edge");
  });

  Scenario("Touch limiting (where the hand is drawn)", [] {
    const std::array panels {PanelA};
    // How far the deepest drawn fingertip goes into the surface
    const auto deepest = [](const HandJoints& hand) {
      float ret = -1;
      for (const auto tip: {Joint::ThumbTip, Joint::IndexTip, Joint::MiddleTip, Joint::RingTip, Joint::LittleTip}) {
        const auto& joint = hand[tip];
        ret = std::max(ret, joint.mRadius - ProjectOntoPanel(joint.mPosition, PanelA, Viewer).mDistance);
      }
      return ret;
    };
    const auto same = [](const HandJoints& a, const HandJoints& b) {
      return (a[Joint::IndexTip].mPosition - b[Joint::IndexTip].mPosition).Length() < 1e-6f
        && (a[Joint::Wrist].mPosition - b[Joint::Wrist].mPosition).Length() < 1e-6f;
    };

    const auto through = MakeHand(Pose::Point, PokeBasis, OnA(0, 0, -0.008f));
    const auto drawn = LimitToSurfaces(through, panels, std::nullopt, Viewer);
    Check(std::abs(deepest(drawn)) < 1e-4f, std::format("8mm through: drawn resting on the surface ({:.2f}mm into it)", deepest(drawn) * 1000));
    const auto fingerMove = drawn[Joint::IndexTip].mPosition - through[Joint::IndexTip].mPosition;
    const auto wristMove = drawn[Joint::Wrist].mPosition - through[Joint::Wrist].mPosition;
    Check((fingerMove - wristMove).Length() < 1e-5f && fingerMove.Length() > 0.008f, "the whole drawn hand moves back, not just the fingertip");

    const auto hovering = MakeHand(Pose::Point, PokeBasis, OnA(0, 0, 0.03f));
    Check(same(LimitToSurfaces(hovering, panels, std::nullopt, Viewer), hovering), "3cm in front: drawn where it is");

    const auto reaching = MakeHand(Pose::Point, PokeBasis, OnA(0, 0, -0.12f));
    Check(same(LimitToSurfaces(reaching, panels, std::nullopt, Viewer), reaching), "12cm through (reaching behind the panel): drawn where it is");

    const auto beside = MakeHand(Pose::Point, PokeBasis, OnA(0.35f, 0, -0.008f));
    Check(same(LimitToSurfaces(beside, panels, std::nullopt, Viewer), beside), "beside the panel: drawn where it is");

    const auto onKeyboard = LimitToSurfaces(through, {}, PanelA, Viewer);
    Check(std::abs(deepest(onKeyboard)) < 1e-4f, "the virtual keyboard limits the drawn hand too");

    // NaN from hand tracking must not move the hand anywhere strange
    auto broken = through;
    broken[Joint::IndexTip].mPosition.x = std::numeric_limits<float>::quiet_NaN();
    const auto drawnBroken = LimitToSurfaces(broken, panels, std::nullopt, Viewer);
    Check(std::isfinite(drawnBroken[Joint::Wrist].mPosition.x), "a NaN fingertip doesn't move the drawn hand to NaN");
  });

  Scenario("Pointing poke in the center clicks exactly once", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.02f), 0.3);
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.02f), 0.1);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.02f), OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, 0.3f), 0.3);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 1, std::format("one touch (got {})", r.mTouches));
    Check(r.mReleases == 1, std::format("one release (got {})", r.mReleases));
    Check(r.mHoveredBeforeFirstTouch, "the cursor hovered before the click");
    Check(r.mFirstTouch && std::abs(r.mFirstTouch->mU - 0.5f) < 0.02f && std::abs(r.mFirstTouch->mV - 0.5f) < 0.02f,
      r.mFirstTouch ? std::format("click landed in the center ({:.3f}, {:.3f})", r.mFirstTouch->mU, r.mFirstTouch->mV) : "click landed in the center (no click)");
    Check(!r.mUpdates.empty() && r.mUpdates.back().mState == TouchState::NotNear, "cursor left the panel when the hand moved away");
  });

  Scenario("Poking a corner clicks the corner", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0.12f, -0.07f, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0.12f, -0.07f, 0.05f), OnA(0.12f, -0.07f, -0.01f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0.12f, -0.07f, -0.01f), OnA(0.12f, -0.07f, 0.05f), 0.3);
    const auto& r = sim.mRecorder;
    // u = 0.12 / 0.30 + 0.5 = 0.9; v = 0.5 + 0.07 / 0.20 = 0.85
    Check(r.mTouches == 1, std::format("one touch (got {})", r.mTouches));
    Check(r.mFirstTouch && std::abs(r.mFirstTouch->mU - 0.9f) < 0.02f && std::abs(r.mFirstTouch->mV - 0.85f) < 0.02f,
      r.mFirstTouch ? std::format("click at (0.9, 0.85) (got {:.3f}, {:.3f})", r.mFirstTouch->mU, r.mFirstTouch->mV) : "click at (0.9, 0.85) (no click)");
  });

  Scenario("Flat hand sweeping through the panel never clicks", [] {
    Simulation sim;
    sim.Move(Pose::Flat, PokeBasis, OnA(-0.2f, 0, 0.05f), OnA(0.2f, 0, -0.05f), 1.0);
    sim.Move(Pose::Flat, PokeBasis, OnA(0.2f, 0, -0.05f), OnA(-0.2f, 0.05f, 0.05f), 1.0);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 0, std::format("no touches (got {})", r.mTouches));
    Check(r.mUpdates.empty(), std::format("no cursor at all (got {} updates)", r.mUpdates.size()));
    Check(r.mLastVisibility == 0, "hand stays hidden");
  });

  Scenario("Fist pushed through the panel never clicks", [] {
    Simulation sim;
    sim.Move(Pose::Fist, PokeBasis, OnA(0, 0, 0.10f), OnA(0, 0, -0.05f), 0.5);
    Check(sim.mRecorder.mTouches == 0, "no touches");
    Check(sim.mRecorder.mUpdates.empty(), "no cursor at all");
  });

  Scenario("Relaxed half-open hand never clicks", [] {
    Simulation sim;
    sim.Move(Pose::Relaxed, PokeBasis, OnA(0, 0, 0.10f), OnA(0, 0, -0.03f), 0.5);
    Check(sim.mRecorder.mTouches == 0, "no touches");
  });

  Scenario("Pointing finger sliding in from the side doesn't click", [] {
    Simulation sim;
    // Already 1cm 'through' the panel's plane, but outside its edge
    sim.Hold(Pose::Point, PokeBasis, OnA(0.30f, 0, -0.01f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0.30f, 0, -0.01f), OnA(0, 0, -0.01f), 0.5);
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), 0.2);
    Check(sim.mRecorder.mTouches == 0, std::format("no touch while sliding in (got {})", sim.mRecorder.mTouches));
    // ... but pulling back and tapping properly works
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.3);
    Check(sim.mRecorder.mTouches == 1, std::format("a proper tap afterwards clicks (got {})", sim.mRecorder.mTouches));
  });

  Scenario("Pointing finger coming from behind the panel doesn't click", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.06f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.06f), OnA(0, 0, 0.05f), 0.5);
    Check(sim.mRecorder.mTouches == 0, std::format("no touches (got {})", sim.mRecorder.mTouches));
  });

  Scenario("Finger pointing along the panel (not into it) doesn't click", [] {
    Simulation sim;
    sim.Hold(Pose::Point, SidewaysBasis, OnA(0.05f, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, SidewaysBasis, OnA(0.05f, 0, 0.05f), OnA(0.05f, 0, -0.01f), 0.3);
    sim.Hold(Pose::Point, SidewaysBasis, OnA(0.05f, 0, -0.01f), 0.2);
    Check(sim.mRecorder.mTouches == 0, std::format("no touches (got {})", sim.mRecorder.mTouches));
  });

  Scenario("Jitter while hovering doesn't click; jitter while touching doesn't double-click", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, 0.015f), 0.2);
    // +/- 3mm on every axis, 1.5cm in front
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.015f), OnA(0, 0, 0.015f), 2.0, 0.003f);
    Check(sim.mRecorder.mTouches == 0, std::format("hovering at 1.5cm: no touches (got {})", sim.mRecorder.mTouches));
    // ... now 3mm in front, with the same jitter
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.015f), OnA(0, 0, 0.003f), 0.1);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.003f), OnA(0, 0, 0.003f), 1.0, 0.003f);
    Check(sim.mRecorder.mTouches == 1, std::format("touching at 0.3cm: exactly one touch (got {})", sim.mRecorder.mTouches));
    Check(sim.mRecorder.mReleases == 0, std::format("... and no releases (got {})", sim.mRecorder.mReleases));
  });

  Scenario("Small wobble (6mm) while touching keeps the click in place", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.005f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.005f), OnA(0.005f, 0.003f, -0.005f), 0.3);
    const auto& r = sim.mRecorder;
    Check(r.mFirstTouch && r.mLastTouch && std::abs(r.mLastTouch->mU - r.mFirstTouch->mU) < 0.001f && std::abs(r.mLastTouch->mV - r.mFirstTouch->mV) < 0.001f,
      "click position didn't move");
  });

  Scenario("Deliberate drag while touching moves the cursor", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.005f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.005f), OnA(0.05f, 0, -0.005f), 0.5);
    sim.Hold(Pose::Point, PokeBasis, OnA(0.05f, 0, -0.005f), 0.5);
    const auto& r = sim.mRecorder;
    // 0.5 + 0.05 / 0.30
    Check(r.mLastTouch && std::abs(r.mLastTouch->mU - 0.667f) < 0.03f,
      r.mLastTouch ? std::format("dragged to u=0.667 (got {:.3f})", r.mLastTouch->mU) : "dragged (no touch)");
    Check(r.mReleases == 0, "still touching");
  });

  // Reaching down to a panel: the finger comes in at about 39 degrees from
  // straight-in, moving down the panel, and meets it at y = 0
  const auto angledPoke = [](const Settings& settings, float throughBy) {
    Simulation sim({PanelA}, settings);
    const float slope = 0.8f;
    const auto at = [&](float distanceInFront) {
      return OnA(0, slope * distanceInFront, distanceInFront);
    };
    sim.Hold(Pose::Point, PokeBasis, at(0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, at(0.05f), at(-throughBy), 0.05 + (2.5 * (0.05 + throughBy)));
    // Pull back out the way it came
    sim.Move(Pose::Point, PokeBasis, at(-throughBy), at(0.05f), 0.25);
    return sim.mRecorder;
  };
  const auto angledMissMM = [](const Recorder& r) {
    return r.mFirstTouch ? std::abs(r.mFirstTouch->mV - 0.5f) * 0.20f * 1000 : 1000.0f;
  };

  Scenario("Coming in at an angle, the click lands where the finger meets the panel", [&] {
    Settings straightOnto;
    straightOnto.mMaxApproachSlope = 0;
    const auto before = angledMissMM(angledPoke(straightOnto, 0.01f));
    const auto r = angledPoke({}, 0.01f);
    Check(r.mTouches == 1 && r.mReleases == 1, std::format("one click ({} down, {} up)", r.mTouches, r.mReleases));
    Check(
      angledMissMM(r) < 2.0f,
      std::format(
        "click is {:.1f}mm from where the finger met the panel (limit 2mm; "
        "straight onto the panel, it was {:.1f}mm above)",
        angledMissMM(r),
        before));
  });

  Scenario("Pushing in further at an angle, then pulling back, is still a tap", [&] {
    const auto r = angledPoke({}, 0.025f);
    Check(r.mTouches == 1 && r.mReleases == 1, std::format("one click ({} down, {} up)", r.mTouches, r.mReleases));
    Check(
      r.mFirstTouch && r.mFirstRelease
        && std::abs(r.mFirstRelease->mU - r.mFirstTouch->mU) < 1e-5f
        && std::abs(r.mFirstRelease->mV - r.mFirstTouch->mV) < 1e-5f,
      "it didn't turn into a drag: released where it was pressed");
    Check(angledMissMM(r) < 2.0f, std::format("click is {:.1f}mm from where the finger met the panel (limit 2mm)", angledMissMM(r)));
  });

  // Largest pointer movement between two updates while touching, in mm
  const auto largestStepMM = [](const Recorder& r) {
    float largest = 0;
    std::optional<CursorUpdate> previous;
    for (const auto& update: r.mUpdates) {
      if (update.mState != TouchState::Touching) {
        previous.reset();
        continue;
      }
      if (previous) {
        const float du = (update.mU - previous->mU) * metersPerU;
        const float dv = (update.mV - previous->mV) * 0.20f;
        largest = std::max(largest, std::sqrt((du * du) + (dv * dv)) * 1000);
      }
      previous = update;
    }
    return largest;
  };

  Scenario("Press, hold, then drag a slider while the hand drifts back", [&] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.012f), 0.2);
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.012f), 0.4);
    // 8cm across in 0.8s, drifting 1cm back toward the front of the panel
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.012f), OnA(0.08f, 0, -0.002f), 0.8);
    sim.Hold(Pose::Point, PokeBasis, OnA(0.08f, 0, -0.002f), 0.1);
    const auto& r = sim.mRecorder;
    const auto beforeLettingGo = r.mLastTouch;
    Check(r.mTouches == 1 && r.mReleases == 0, std::format("still holding ({} down, {} up)", r.mTouches, r.mReleases));
    const float targetU = 0.5f + (0.08f / metersPerU);
    Check(
      beforeLettingGo && std::abs(beforeLettingGo->mU - targetU) * metersPerU < 0.003f,
      beforeLettingGo
        ? std::format("dragged to the finger ({:.1f}mm away, limit 3mm)", std::abs(beforeLettingGo->mU - targetU) * metersPerU * 1000)
        : "dragged (no touch)");
    Check(largestStepMM(r) < 4.0f, std::format("the drag started smoothly (largest step {:.1f}mm, limit 4mm)", largestStepMM(r)));

    // Let go: pull back quickly, moving 1cm sideways as hands do
    sim.Move(Pose::Point, PokeBasis, OnA(0.08f, 0, -0.002f), OnA(0.09f, 0, 0.05f), 0.15);
    Check(r.mReleases == 1, std::format("pulling back let go ({} up)", r.mReleases));
    Check(
      r.mFirstRelease && beforeLettingGo
        && std::abs(r.mFirstRelease->mU - beforeLettingGo->mU) * metersPerU < 0.003f,
      r.mFirstRelease && beforeLettingGo
        ? std::format(
            "letting go didn't move the slider much ({:.1f}mm, limit 3mm)",
            std::abs(r.mFirstRelease->mU - beforeLettingGo->mU) * metersPerU * 1000)
        : "let go (no release)");
  });

  Scenario("Press then drag straight away, easing off as the drag starts", [&] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.010f), 0.15);
    // 5mm of easing off in the first 0.12s, which used to end it as a tap
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.010f), OnA(0.015f, 0, -0.005f), 0.12);
    sim.Move(Pose::Point, PokeBasis, OnA(0.015f, 0, -0.005f), OnA(0.07f, 0, -0.004f), 0.35);
    sim.Hold(Pose::Point, PokeBasis, OnA(0.07f, 0, -0.004f), 0.2);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 1 && r.mReleases == 0, std::format("one drag, still held ({} down, {} up)", r.mTouches, r.mReleases));
    const float targetU = 0.5f + (0.07f / metersPerU);
    Check(
      r.mLastTouch && std::abs(r.mLastTouch->mU - targetU) * metersPerU < 0.003f,
      r.mLastTouch
        ? std::format("dragged to the finger ({:.1f}mm away, limit 3mm)", std::abs(r.mLastTouch->mU - targetU) * metersPerU * 1000)
        : "dragged (no touch)");
    Check(largestStepMM(r) < 4.5f, std::format("the drag started smoothly (largest step {:.1f}mm, limit 4.5mm)", largestStepMM(r)));
  });

  Scenario("Losing tracking mid-tap releases the click", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.3);
    sim.LoseTracking(0.1);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 1 && r.mReleases == 1, std::format("touched then released ({} / {})", r.mTouches, r.mReleases));
    Check(!r.mUpdates.empty() && r.mUpdates.back().mState == TouchState::NotNear, "cursor removed");
  });

  Scenario("Losing the pointing pose mid-tap still finishes the tap cleanly", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.3);
    sim.Hold(Pose::Flat, PokeBasis, OnA(0, 0, -0.01f), 0.3);
    Check(sim.mRecorder.mReleases == 0, "still touching while the finger stays in");
    sim.Move(Pose::Flat, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.3);
    Check(sim.mRecorder.mTouches == 1 && sim.mRecorder.mReleases == 1, "released when pulled back");
  });

  Scenario("Relaxed little finger still counts as pointing", [] {
    Simulation sim;
    sim.Hold(Pose::PointRelaxedLittle, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::PointRelaxedLittle, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.3);
    sim.Move(Pose::PointRelaxedLittle, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.3);
    Check(sim.mRecorder.mTouches == 1, std::format("one touch (got {})", sim.mRecorder.mTouches));
  });

  Scenario("Moving between panels leaves the first one", [] {
    Simulation sim({PanelA, PanelB});
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0.4f, 0, 0.05f), 0.6);
    const auto& updates = sim.mRecorder.mUpdates;
    std::optional<std::size_t> leftA;
    std::optional<std::size_t> firstB;
    for (std::size_t i = 0; i < updates.size(); ++i) {
      if (updates[i].mPanelID == PanelA.mID && updates[i].mState == TouchState::NotNear && !leftA) {
        leftA = i;
      }
      if (updates[i].mPanelID == PanelB.mID && !firstB) {
        firstB = i;
      }
    }
    Check(leftA && firstB && *leftA < *firstB, "'not near' sent to the first panel before the second gets a cursor");
    Check(sim.mRecorder.mTouches == 0, "no touches");
  });

  Scenario("Hand fades in while pointing and out afterwards", [] {
    Simulation sim;
    // 30cm from the panel: no interaction, just the pose
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.30f), 0.05);
    Check(sim.mRecorder.mLastVisibility == 0, "hidden before the pose is held long enough");
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.30f), 0.40);
    Check(sim.mRecorder.mLastVisibility == 1, std::format("fully visible after holding the pose (got {:.2f})", sim.mRecorder.mLastVisibility));
    sim.Hold(Pose::Flat, PokeBasis, OnA(0, 0, 0.30f), 0.50);
    Check(sim.mRecorder.mLastVisibility == 0, std::format("hidden again after a flat hand (got {:.2f})", sim.mRecorder.mLastVisibility));
    Check(sim.mRecorder.mUpdates.empty(), "no cursor at 30cm");
  });

  ///// Responsiveness /////


  for (const float speed: {0.05f, 0.10f}) {
    const auto limit = (speed < 0.07f) ? 0.002f : 0.0035f;
    Scenario(
      std::format(
        "The hover cursor keeps up with a finger moving at {:.0f}cm/s",
        speed * 100)
        .c_str(),
      [speed, limit] {
        Simulation sim;
        sim.Hold(Pose::Point, PokeBasis, OnA(-0.05f, 0, 0.05f), 0.3);
        sim.Move(
          Pose::Point,
          PokeBasis,
          OnA(-0.05f, 0, 0.05f),
          OnA(-0.05f + speed, 0, 0.05f),
          1.0);
        const auto& r = sim.mRecorder;
        const auto lag = r.mLastHover
          ? std::abs(r.mLastHover->mU - r.mTruth.mU) * metersPerU
          : 1.0f;
        Check(
          lag < limit,
          std::format(
            "cursor is {:.1f}mm behind the finger (limit {:.1f}mm)",
            lag * 1000,
            limit * 1000));
      });
  }

  Scenario("The hover cursor stays steady when the finger is still", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.5);
    const auto start = sim.mRecorder.mUpdates.size();
    // +/- 1.5mm of tracking noise on every axis
    sim.Move(
      Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, 0.05f), 2.0, 0.0015f);
    float minU = 1;
    float maxU = 0;
    for (std::size_t i = start; i < sim.mRecorder.mUpdates.size(); ++i) {
      minU = std::min(minU, sim.mRecorder.mUpdates[i].mU);
      maxU = std::max(maxU, sim.mRecorder.mUpdates[i].mU);
    }
    const auto range = (maxU > minU) ? (maxU - minU) * metersPerU : 0.0f;
    Check(
      range < 0.0025f,
      std::format("cursor wanders {:.1f}mm (limit 2.5mm)", range * 1000));
  });

  Scenario("Aim quickly, then tap at once: the click lands under the finger", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(-0.04f, 0, 0.03f), 0.3);
    // A quick 6cm aiming move (30cm/s), then straight into a fast poke
    sim.Move(Pose::Point, PokeBasis, OnA(-0.04f, 0, 0.03f), OnA(0.02f, 0, 0.03f), 0.2);
    sim.Move(Pose::Point, PokeBasis, OnA(0.02f, 0, 0.03f), OnA(0.02f, 0, -0.01f), 0.08);
    sim.Move(Pose::Point, PokeBasis, OnA(0.02f, 0, -0.01f), OnA(0.02f, 0, 0.05f), 0.2);
    const auto& r = sim.mRecorder;
    const float targetU = 0.5f + (0.02f / metersPerU);
    const auto error = r.mFirstTouch
      ? std::abs(r.mFirstTouch->mU - targetU) * metersPerU
      : 1.0f;
    Check(r.mTouches == 1, std::format("one touch (got {})", r.mTouches));
    Check(
      error < 0.002f,
      std::format(
        "click is {:.1f}mm from where the finger touched (limit 2mm)",
        error * 1000));
  });

  Scenario("Repeated taps on one spot, with tracking noise, land together", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0.05f, 0.03f, 0.03f), 0.3);
    for (int i = 0; i < 8; ++i) {
      sim.Move(Pose::Point, PokeBasis, OnA(0.05f, 0.03f, 0.03f), OnA(0.05f, 0.03f, 0.03f), 0.15, 0.0015f);
      sim.Move(Pose::Point, PokeBasis, OnA(0.05f, 0.03f, 0.03f), OnA(0.05f, 0.03f, -0.01f), 0.1, 0.0015f);
      sim.Move(Pose::Point, PokeBasis, OnA(0.05f, 0.03f, -0.01f), OnA(0.05f, 0.03f, 0.03f), 0.1, 0.0015f);
    }
    const auto& r = sim.mRecorder;
    const float targetU = 0.5f + (0.05f / metersPerU);
    const float targetV = 0.5f - (0.03f / 0.20f);
    float worst = 0;
    for (const auto& down: r.mTouchDowns) {
      const float du = (down.mU - targetU) * metersPerU;
      const float dv = (down.mV - targetV) * 0.20f;
      worst = std::max(worst, std::sqrt((du * du) + (dv * dv)));
    }
    Check(r.mTouches == 8, std::format("eight taps (got {})", r.mTouches));
    Check(
      worst < 0.0025f,
      std::format("furthest click is {:.1f}mm off target (limit 2.5mm)", worst * 1000));
  });

  Scenario("A tap finishes as soon as the finger starts pulling back", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.015f), 0.2);
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.015f), 0.1);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.015f), OnA(0, 0, 0.05f), 0.4);
    const auto& r = sim.mRecorder;
    Check(
      r.mFirstReleaseTipDistance && *r.mFirstReleaseTipDistance < 0,
      r.mFirstReleaseTipDistance
        ? std::format(
            "released with the fingertip still {:.1f}cm through the panel "
            "(it used to need 2cm in front)",
            -*r.mFirstReleaseTipDistance * 100)
        : "released (no release)");
  });

  Scenario("A click is released exactly where it was pressed", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.005f), 0.2);
    // Drift sideways a little (not a drag) before pulling back
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.005f), OnA(0.009f, -0.005f, -0.005f), 0.2);
    sim.Move(Pose::Point, PokeBasis, OnA(0.009f, -0.005f, -0.005f), OnA(0.009f, -0.005f, 0.05f), 0.3);
    const auto& r = sim.mRecorder;
    Check(
      r.mFirstTouch && r.mFirstRelease
        && std::abs(r.mFirstRelease->mU - r.mFirstTouch->mU) < 1e-5f
        && std::abs(r.mFirstRelease->mV - r.mFirstTouch->mV) < 1e-5f,
      "button-up is at the same spot as button-down");
  });

  Scenario("A quick double tap is two clicks", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.12);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.018f), 0.12);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.018f), OnA(0, 0, -0.01f), 0.12);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.2);
    const auto& r = sim.mRecorder;
    Check(
      r.mTouches == 2 && r.mReleases == 2,
      std::format("two clicks ({} down, {} up)", r.mTouches, r.mReleases));
  });

  Scenario("Resting a finger on the panel, with tracking noise, holds the click", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.005f), 0.2);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.005f), OnA(0, 0, -0.005f), 1.5, 0.002f);
    const auto& r = sim.mRecorder;
    Check(
      r.mTouches == 1 && r.mReleases == 0,
      std::format("still one held click ({} down, {} up)", r.mTouches, r.mReleases));
  });

  // For comparison only: the first version's tuning. Its taps used the
  // hover cursor's smoothing, and only ended 2cm in front of the panel.
  ///// Two-finger scrolling /////

  Scenario("Two fingers out shows the hand, and touching doesn't click", [] {
    Simulation sim;
    sim.Hold(Pose::TwoFingers, PokeBasis, OnA(0, 0, 0.05f), 0.4);
    Check(sim.mRecorder.mLastVisibility == 1, "hand is shown");
    sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.2);
    sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.2);
    Check(sim.mRecorder.mTouches == 0, std::format("no clicks (got {})", sim.mRecorder.mTouches));
    Check(sim.mRecorder.mWheelTotal == 0, std::format("no scrolling without dragging (got {})", sim.mRecorder.mWheelTotal));
    Check(sim.mRecorder.mHoveredBeforeFirstTouch, "the cursor hovered");
  });

  for (const float direction: {1.0f, -1.0f}) {
    Scenario(
      direction > 0
        ? "Dragging two fingers down 6.6cm scrolls 5 notches up (content follows the fingers)"
        : "Dragging two fingers up 6.6cm scrolls 5 notches down",
      [direction] {
        Simulation sim;
        // 5.5 notches of 1.2cm; only whole notches are sent
        const float start = 0.033f * direction;
        const float end = -0.033f * direction;
        sim.Hold(Pose::TwoFingers, PokeBasis, OnA(0, start, 0.05f), 0.3);
        sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, start, 0.05f), OnA(0, start, -0.005f), 0.2);
        sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, start, -0.005f), OnA(0, end, -0.005f), 0.5);
        sim.Hold(Pose::TwoFingers, PokeBasis, OnA(0, end, -0.005f), 0.2);
        sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, end, -0.005f), OnA(0, end, 0.05f), 0.2);
        const auto& r = sim.mRecorder;
        const int expected = static_cast<int>(direction * 600);
        Check(
          r.mWheelTotal == expected,
          std::format("wheel moved {} (expected {})", r.mWheelTotal, expected));
        Check(r.mTouches == 0, std::format("no clicks (got {})", r.mTouches));
        bool pointerStayed = true;
        for (const auto& update: r.mWheelUpdates) {
          pointerStayed &= std::abs(update.mU - r.mWheelUpdates.front().mU) < 1e-5f
            && std::abs(update.mV - r.mWheelUpdates.front().mV) < 1e-5f;
        }
        Check(pointerStayed, "the pointer stays where the fingers first touched");
      });
  }

  Scenario("Switching from pointing to two fingers while hovering doesn't click", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Hold(Pose::TwoFingers, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.2);
    sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.2);
    Check(sim.mRecorder.mTouches == 0, std::format("no clicks (got {})", sim.mRecorder.mTouches));
  });

  Scenario("Pointing still taps after scrolling", [] {
    Simulation sim;
    sim.Hold(Pose::TwoFingers, PokeBasis, OnA(0, 0.03f, 0.05f), 0.3);
    sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, 0.03f, 0.05f), OnA(0, 0.03f, -0.005f), 0.2);
    sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, 0.03f, -0.005f), OnA(0, -0.03f, -0.005f), 0.5);
    sim.Move(Pose::TwoFingers, PokeBasis, OnA(0, -0.03f, -0.005f), OnA(0, -0.03f, 0.05f), 0.2);
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.2);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.2);
    Check(sim.mRecorder.mWheelTotal != 0, "scrolled");
    Check(sim.mRecorder.mTouches == 1, std::format("then one click (got {})", sim.mRecorder.mTouches));
  });

  Scenario("A flat hand doesn't scroll", [] {
    Simulation sim;
    sim.Move(Pose::Flat, PokeBasis, OnA(0, 0.04f, 0.05f), OnA(0, 0.04f, -0.01f), 0.3);
    sim.Move(Pose::Flat, PokeBasis, OnA(0, 0.04f, -0.01f), OnA(0, -0.04f, -0.01f), 0.5);
    Check(sim.mRecorder.mWheelTotal == 0 && sim.mRecorder.mTouches == 0, "nothing happened");
  });

  // Only one hand presses at a time
  Scenario("Two hands on one panel: one press, never handed to the other finger", [] {
    Simulation sim;
    // Left finger on the left side of the panel (u = 0.23), right finger on the
    // right side (u = 0.77)
    const auto leftOut = OnA(-0.08f, 0, 0.05f);
    const auto leftIn = OnA(-0.08f, 0, -0.01f);
    const auto rightOut = OnA(0.08f, 0, 0.05f);
    const auto rightIn = OnA(0.08f, 0, -0.01f);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightOut, rightOut, 0.3);
    // Left presses; while it's held, right pushes in too; then left lets go
    // while right is still in, then right pulls back
    sim.MoveBoth(Pose::Point, leftOut, leftIn, rightOut, rightOut, 0.3);
    sim.MoveBoth(Pose::Point, leftIn, leftIn, rightOut, rightIn, 0.3);
    sim.MoveBoth(Pose::Point, leftIn, leftOut, rightIn, rightIn, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightIn, rightIn, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightIn, rightOut, 0.3);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 1, std::format("one press (got {})", r.mTouches));
    Check(r.mReleases == 1, std::format("one release (got {})", r.mReleases));
    Check(
      std::ranges::all_of(r.mUpdates, [](const CursorUpdate& u) {
        return u.mState != TouchState::Touching || u.mU < 0.5f;
      }),
      "the press stayed with the left finger (no drag to the right finger)");
    Check(r.mFirstTouch && r.mFirstRelease && std::abs(r.mFirstRelease->mU - r.mFirstTouch->mU) < 0.01f,
      "released where it was pressed");
    // Having pulled back, the right hand taps normally
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightOut, rightIn, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightIn, rightOut, 0.3);
    Check(r.mTouches == 2 && r.mReleases == 2, std::format("the right hand then taps once ({} / {})", r.mTouches, r.mReleases));
    Check(r.mTouchDowns.size() == 2 && r.mTouchDowns[1].mU > 0.5f, "... on the right side");
  });

  Scenario("Two hands on different panels: no delayed click on the second panel", [] {
    Simulation sim({PanelA, PanelB});
    const auto leftOut = OnA(0, 0, 0.05f);
    const auto leftIn = OnA(0, 0, -0.01f);
    const auto rightOut = OnA(0.4f, 0, 0.05f);
    const auto rightIn = OnA(0.4f, 0, -0.01f);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightOut, rightOut, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftIn, rightOut, rightOut, 0.3);
    sim.MoveBoth(Pose::Point, leftIn, leftIn, rightOut, rightIn, 0.3);
    sim.MoveBoth(Pose::Point, leftIn, leftOut, rightIn, rightIn, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightIn, rightIn, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightIn, rightOut, 0.3);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 1 && r.mReleases == 1, std::format("one press and release ({} / {})", r.mTouches, r.mReleases));
    Check(
      std::ranges::none_of(r.mUpdates, [](const CursorUpdate& u) {
        return u.mPanelID == PanelB.mID && u.mState == TouchState::Touching;
      }),
      "the second panel never got a press");
  });

  Scenario("Where panels overlap exactly, a tap goes to the one drawn on top", [] {
    Panel top = PanelA;
    top.mID = 0xC;
    // Panels are in draw order: `top` is drawn last
    Simulation sim({PanelA, top});
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.3);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 1, std::format("one touch (got {})", r.mTouches));
    Check(r.mFirstTouch && r.mFirstTouch->mPanelID == top.mID, "the touch went to the panel on top");
  });

  Scenario("Broken tracking data (NaN) is ignored, and taps work afterwards", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    auto broken = MakeHand(Pose::Point, PokeBasis, OnA(0, 0, 0.05f));
    broken[Joint::IndexTip].mPosition.x = std::numeric_limits<float>::quiet_NaN();
    broken[Joint::Wrist].mRadius = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 5; ++i) {
      sim.Step(broken);
    }
    sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.01f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.05f), 0.3);
    const auto& r = sim.mRecorder;
    Check(r.mTouches == 1 && r.mReleases == 1, std::format("one tap afterwards ({} / {})", r.mTouches, r.mReleases));
    Check(r.mFirstTouch && std::abs(r.mFirstTouch->mU - 0.5f) < 0.02f && std::abs(r.mFirstTouch->mV - 0.5f) < 0.02f, "in the center");
    Check(
      std::ranges::all_of(r.mUpdates, [](const CursorUpdate& u) {
        return std::isfinite(u.mU) && std::isfinite(u.mV);
      }),
      "every cursor position sent is a number");
  });

  Scenario("Stress test: noisy holds, 50 random seeds each", [] {
    int heldOnPanel = 0;
    int heldInFront = 0;
    int extraClicks = 0;
    int drags = 0;
    const auto dragged = [](const Recorder& r) {
      return r.mFirstTouch && r.mLastTouch
        && (std::abs(r.mLastTouch->mU - r.mFirstTouch->mU) > 1e-4f
            || std::abs(r.mLastTouch->mV - r.mFirstTouch->mV) > 1e-4f);
    };
    for (uint32_t seed = 1; seed <= 50; ++seed) {
      {
        // Resting on the panel with 2mm of noise, from the moment of touch
        Simulation sim({PanelA}, {}, seed);
        sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
        sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.005f), 0.2);
        sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.005f), OnA(0, 0, -0.005f), 1.5, 0.002f);
        heldOnPanel += (sim.mRecorder.mReleases == 0);
        extraClicks += std::max(0, sim.mRecorder.mTouches - 1);
        drags += dragged(sim.mRecorder);
      }
      {
        // 3mm in front with 3mm of noise, after noisy hovering
        Simulation sim({PanelA}, {}, seed);
        sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
        sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, 0.015f), 0.2);
        sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.015f), OnA(0, 0, 0.015f), 2.0, 0.003f);
        sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.015f), OnA(0, 0, 0.003f), 0.1);
        sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.003f), OnA(0, 0, 0.003f), 1.0, 0.003f);
        heldInFront += (sim.mRecorder.mReleases == 0);
        extraClicks += std::max(0, sim.mRecorder.mTouches - 1);
        drags += dragged(sim.mRecorder);
      }
    }
    Check(extraClicks == 0, std::format("no double clicks in 100 noisy runs (got {})", extraClicks));
    Check(drags == 0, std::format("noise never turned a held click into a drag (got {})", drags));
    Check(heldOnPanel == 50, std::format("resting on the panel held the click in {} of 50 runs", heldOnPanel));
    Check(heldInFront == 50, std::format("holding 3mm in front held the click in {} of 50 runs", heldInFront));
  });

  std::printf("\nFirst version -> previous version -> now\n");
  {
    // The first version
    Settings before;
    before.mCursorMinCutoff = 1.5f;
    before.mCursorBeta = 8.0f;
    before.mTouchMinCutoff = 1.5f;
    before.mTouchBeta = 8.0f;
    before.mDepthMinCutoff = 6.0f;
    before.mDepthBeta = 10.0f;
    before.mDerivativeCutoff = 1.0f;
    before.mFastRetractDistance = 1.0f;
    before.mRetractDistance = 1.0f;
    before.mArmDelay = 0.10f;
    before.mFadeDuration = 0.15f;

    // The previous version: distance from the 'touch' filter, and a fixed
    // 8mm pull-back to end a tap
    Settings previous;
    previous.mDepthMinCutoff = 8.0f;
    previous.mDepthBeta = 20.0f;
    previous.mFastRetractDistance = 0.008f;

    const auto cursorLag = [&](const Settings& settings, float speed) {
      Simulation sim({PanelA}, settings);
      sim.Hold(Pose::Point, PokeBasis, OnA(-0.05f, 0, 0.05f), 0.3);
      sim.Move(Pose::Point, PokeBasis, OnA(-0.05f, 0, 0.05f), OnA(-0.05f + speed, 0, 0.05f), 1.0);
      const auto& r = sim.mRecorder;
      return std::abs(r.mLastHover->mU - r.mTruth.mU) * metersPerU * 1000;
    };
    const auto quickTapError = [&](const Settings& settings) {
      Simulation sim({PanelA}, settings);
      sim.Hold(Pose::Point, PokeBasis, OnA(-0.04f, 0, 0.03f), 0.3);
      sim.Move(Pose::Point, PokeBasis, OnA(-0.04f, 0, 0.03f), OnA(0.02f, 0, 0.03f), 0.2);
      sim.Move(Pose::Point, PokeBasis, OnA(0.02f, 0, 0.03f), OnA(0.02f, 0, -0.01f), 0.08);
      sim.Move(Pose::Point, PokeBasis, OnA(0.02f, 0, -0.01f), OnA(0.02f, 0, 0.05f), 0.2);
      return std::abs(sim.mRecorder.mFirstTouch->mU - (0.5f + (0.02f / metersPerU))) * metersPerU * 1000;
    };
    const auto releaseDepth = [&](const Settings& settings) {
      Simulation sim({PanelA}, settings);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
      sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.015f), 0.2);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.015f), 0.1);
      sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.015f), OnA(0, 0, 0.05f), 0.4);
      return *sim.mRecorder.mFirstReleaseTipDistance * 100;
    };
    // Time from hand tracking first reporting the pull-back (at 25cm/s) to
    // the click finishing
    const auto releaseDelay = [&](const Settings& settings) {
      Simulation sim({PanelA}, settings);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
      sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.015f), 0.2);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.015f), 0.1);
      int frame = 0;
      while (sim.mRecorder.mReleases == 0 && frame < 200) {
        // Frame 0 is the first one where the fingertip has moved back
        const float d = -0.015f + (0.25f * (frame + 1) / 90.0f);
        sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, d), 1.0 / 90);
        if (sim.mRecorder.mReleases > 0) {
          break;
        }
        ++frame;
      }
      return frame * 1000.0f / 90;
    };

    // Time from hand tracking first reporting the fingertip within 1cm of the
    // panel (where a tap starts) to the click starting, poking in at 25cm/s
    const auto pressDelay = [&](const Settings& settings) {
      Simulation sim({PanelA}, settings);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
      int frame = 0;
      std::optional<int> crossedAt;
      while (frame < 200) {
        const float d = 0.05f - (0.25f * frame / 90.0f);
        sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, d), 1.0 / 90);
        if (!crossedAt && d <= settings.mTouchDistance) {
          crossedAt = frame;
        }
        if (sim.mRecorder.mTouches > 0) {
          break;
        }
        ++frame;
      }
      return (frame - crossedAt.value_or(frame)) * 1000.0f / 90;
    };

    const Settings after;
    std::printf(
      "  time to start a click once you reach the panel: %.0fms -> %.0fms -> "
      "%.0fms\n",
      pressDelay(before),
      pressDelay(previous),
      pressDelay(after));
    std::printf(
      "  time to finish a click once you pull back:      %.0fms -> %.0fms -> "
      "%.0fms\n",
      releaseDelay(before),
      releaseDelay(previous),
      releaseDelay(after));
    std::printf(
      "  click finishes at: %+.1fcm -> %+.1fcm -> %+.1fcm from the panel "
      "(negative = still pressed in)\n",
      releaseDepth(before),
      releaseDepth(previous),
      releaseDepth(after));
    std::printf(
      "  cursor lag at 5cm/s:    %.1fmm -> %.1fmm -> %.1fmm\n",
      cursorLag(before, 0.05f),
      cursorLag(previous, 0.05f),
      cursorLag(after, 0.05f));
    std::printf(
      "  cursor lag at 10cm/s:   %.1fmm -> %.1fmm -> %.1fmm\n",
      cursorLag(before, 0.10f),
      cursorLag(previous, 0.10f),
      cursorLag(after, 0.10f));
    std::printf(
      "  quick aim-and-tap miss: %.1fmm -> %.1fmm -> %.1fmm\n",
      quickTapError(before),
      quickTapError(previous),
      quickTapError(after));

    // The last build: taps straight onto the panel; holds after 0.5s, ended
    // by a fixed 8mm pull-back from the deepest point; drags after 1.5cm,
    // jumping to the finger
    Settings lastBuild;
    lastBuild.mMaxApproachSlope = 0;
    lastBuild.mLongPressTime = 0.5f;
    lastBuild.mHoldMoveDistance = 1.0f;
    lastBuild.mHoldRetractDistance = 0.008f;
    lastBuild.mHoldRelaxTime = 1e9f;
    lastBuild.mHoldRetractPerSpeed = 0;
    lastBuild.mHoldReleaseDistance = 0.02f;
    lastBuild.mRetractFreezeSpeed = 1e9f;
    lastBuild.mDragStartDistance = 0.015f;
    lastBuild.mHoldDragStartDistance = 0.015f;
    lastBuild.mDragCatchUpTime = 1e-6f;
    lastBuild.mDragApproachFadeTime = 1e-6f;

    const auto pressAndDrag = [&](const Settings& settings) {
      Simulation sim({PanelA}, settings);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
      sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.010f), 0.15);
      sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.010f), OnA(0.015f, 0, -0.005f), 0.12);
      sim.Move(Pose::Point, PokeBasis, OnA(0.015f, 0, -0.005f), OnA(0.07f, 0, -0.004f), 0.35);
      return sim.mRecorder;
    };
    const auto driftingSlider = [&](const Settings& settings) {
      Simulation sim({PanelA}, settings);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), 0.3);
      sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.05f), OnA(0, 0, -0.012f), 0.2);
      sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, -0.012f), 0.4);
      sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.012f), OnA(0.08f, 0, -0.002f), 0.8);
      return sim.mRecorder;
    };
    const auto keptHolding = [](const Recorder& r) {
      return (r.mTouches == 1 && r.mReleases == 0) ? "kept holding" : "let go early";
    };
    std::printf("\nLast build -> now\n");
    std::printf(
      "  angled poke (reaching down) lands: %.1fmm -> %.1fmm from where the "
      "finger meets the panel\n",
      angledMissMM(angledPoke(lastBuild, 0.01f)),
      angledMissMM(angledPoke(after, 0.01f)));
    std::printf(
      "  press and drag straight away, easing off: %s -> %s\n",
      keptHolding(pressAndDrag(lastBuild)),
      keptHolding(pressAndDrag(after)));
    std::printf(
      "  hold then drag, hand drifting 1cm back: %s -> %s\n",
      keptHolding(driftingSlider(lastBuild)),
      keptHolding(driftingSlider(after)));
    std::printf(
      "  largest pointer jump as a drag starts: %.1fmm -> %.1fmm\n",
      largestStepMM(driftingSlider(lastBuild)),
      largestStepMM(driftingSlider(after)));
    // Straight-in taps with tracking noise, averaged over seeds: the approach
    // direction must not add much scatter
    const auto noisyTapSpread = [&](const Settings& settings) {
      float total = 0;
      int count = 0;
      for (uint32_t seed = 1; seed <= 20; ++seed) {
        Simulation sim({PanelA}, settings, seed);
        sim.Hold(Pose::Point, PokeBasis, OnA(0, 0, 0.03f), 0.3);
        for (int i = 0; i < 5; ++i) {
          sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.03f), OnA(0, 0, 0.03f), 0.15, 0.0015f);
          sim.Move(Pose::Point, PokeBasis, OnA(0, 0, 0.03f), OnA(0, 0, -0.01f), 0.1, 0.0015f);
          sim.Move(Pose::Point, PokeBasis, OnA(0, 0, -0.01f), OnA(0, 0, 0.03f), 0.1, 0.0015f);
        }
        for (const auto& down: sim.mRecorder.mTouchDowns) {
          const float du = (down.mU - 0.5f) * metersPerU;
          const float dv = (down.mV - 0.5f) * 0.20f;
          total += std::sqrt((du * du) + (dv * dv));
          ++count;
        }
      }
      return count ? (total / count) * 1000 : 0.0f;
    };
    const auto spreadBefore = noisyTapSpread(lastBuild);
    const auto spreadAfter = noisyTapSpread(after);
    std::printf(
      "  straight taps with 1.5mm tracking noise, average miss: %.2fmm -> "
      "%.2fmm\n",
      spreadBefore,
      spreadAfter);
    Check(
      spreadAfter < spreadBefore + 0.3f,
      "following the approach adds little scatter to noisy straight taps");

    // Regression guards for responsiveness, in 90Hz frames
    Check(
      pressDelay(after) < 1.0f,
      "a tap starts in the same frame hand tracking reports the touch");
    Check(
      releaseDelay(after) < 12.0f,
      "a tap finishes within one frame of hand tracking reporting the "
      "pull-back");
  }

  std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
