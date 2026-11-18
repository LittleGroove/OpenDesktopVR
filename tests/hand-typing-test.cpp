// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Headset-free tests for typing on a virtual keyboard with the index
// fingertips, using synthetic hands. Run it from a terminal; it exits with 0
// if every scenario passes.

#include "synthetic-hands.hpp"

#include <OpenDesktopVR/HandTyping.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace OpenDesktopVR::HandInteraction;
using namespace OpenDesktopVR::SyntheticHands;

namespace {

constexpr double FrameTime = 1.0 / 90;

const Vector3 Viewer {0, 1.2f, 0};

// The keyboard is 30cm x 12cm, upright, facing the viewer
constexpr float KeyboardWidth = 0.30f;
constexpr float KeyboardHeight = 0.12f;
const Panel Keyboard {
  .mID = 0x4B,
  .mPosition = {0, 1.0f, -0.5f},
  .mOrientation = Quaternion::Identity,
  .mSize = {KeyboardWidth, KeyboardHeight},
};

// Right hand, palm toward the keyboard (-Z), fingers pointing up
const HandBasis PalmOnBasis {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
// Right hand, fingers pointing down, palm toward the viewer
const HandBasis DownBasis {{-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};

// `x` right of and `y` above the keyboard's center, `distanceInFront` in
// front of it
Vector3 OnKeyboard(float x, float y, float distanceInFront) {
  return {x, 1.0f + y, -0.5f + distanceInFront};
}

// How far (in mm) a press is from the point `x`, `y` on the keyboard
float MissMM(const TypingFinger& finger, float x, float y) {
  const float du = (finger.mU - (0.5f + (x / KeyboardWidth))) * KeyboardWidth;
  const float dv = (finger.mV - (0.5f - (y / KeyboardHeight))) * KeyboardHeight;
  return std::sqrt((du * du) + (dv * dv)) * 1000;
}

int gFailures = 0;
int gChecks = 0;
// Frames where `mChanged` didn't match whether the result changed, over every
// simulation
int gChangedMismatches = 0;
int gFrames = 0;

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

struct FingerRecord {
  int mPresses {0};
  int mReleases {0};
  // Presses counted from `mPressCount`
  int mCountedPresses {0};
  std::vector<TypingFinger> mPressDowns;
  // How far in front of the keyboard the real fingertip was when each press
  // started, and ended
  std::vector<float> mPressTipDistances;
  std::vector<float> mReleaseTipDistances;
  TypingFinger mLast {};
  // A press moved while held
  bool mPressMoved {false};
  bool mHovered {false};
  bool mWasNear {false};
  int mFramesPressed {0};
};

class Simulation {
 public:
  explicit Simulation(
    const TypingSettings& settings = {},
    uint32_t seed = 12345,
    std::optional<Panel> keyboard = Keyboard)
    : mTracker(settings),
      mKeyboard(keyboard),
      mRandom(seed) {
  }

  // Moves the right hand's index fingertip in a straight line; the left hand
  // isn't tracked
  void Move(
    Pose pose,
    const HandBasis& basis,
    Vector3 from,
    Vector3 to,
    double seconds,
    float noise = 0) {
    const int frames = FramesFor(seconds);
    for (int i = 0; i < frames; ++i) {
      const float t = static_cast<float>(i + 1) / frames;
      Step(
        HandJoints {},
        MakeHand(pose, basis, Vector3::Lerp(from, to, t) + Noise(noise)));
    }
  }

  void Hold(
    Pose pose,
    const HandBasis& basis,
    Vector3 tip,
    double seconds,
    float noise = 0) {
    Move(pose, basis, tip, tip, seconds, noise);
  }

  // Moves both index fingertips in straight lines, at the same time
  void MoveBoth(
    Pose pose,
    Vector3 leftFrom,
    Vector3 leftTo,
    Vector3 rightFrom,
    Vector3 rightTo,
    double seconds,
    float noise = 0) {
    const int frames = FramesFor(seconds);
    for (int i = 0; i < frames; ++i) {
      const float t = static_cast<float>(i + 1) / frames;
      const auto left = Vector3::Lerp(leftFrom, leftTo, t) + Noise(noise);
      const auto right = Vector3::Lerp(rightFrom, rightTo, t) + Noise(noise);
      Step(
        MakeHand(pose, LeftPokeBasis, left), MakeHand(pose, PokeBasis, right));
    }
  }

  void LoseTracking(double seconds) {
    const int frames = FramesFor(seconds);
    for (int i = 0; i < frames; ++i) {
      Step(HandJoints {}, HandJoints {});
    }
  }

  // No updates at all for this long, e.g. the game is paused
  void Pause(double seconds) {
    mNow += seconds;
  }

  void Step(const HandJoints& left, const HandJoints& right) {
    mNow += FrameTime;
    Record(mTracker.Update(mNow, {left, right}, mKeyboard, Viewer), {left, right});
  }

  void Reset() {
    Record(mTracker.Reset(), {});
  }

  [[nodiscard]]
  const TypingDiagnostics& Diagnostics(Hand hand) const {
    return mTracker.GetDiagnostics()[static_cast<std::size_t>(hand)];
  }

  [[nodiscard]]
  const FingerRecord& Right() const {
    return mFingers[1];
  }
  [[nodiscard]]
  const FingerRecord& Left() const {
    return mFingers[0];
  }

  std::array<FingerRecord, HandCount> mFingers {};
  TypingResult mLast {};
  int mFrames {0};
  int mUnchangedFrames {0};

 private:
  TypingTracker mTracker;
  std::optional<Panel> mKeyboard;
  double mNow {1000};
  std::mt19937 mRandom;
  std::uniform_real_distribution<float> mNoise {-1.0f, 1.0f};

  static int FramesFor(double seconds) {
    return std::max(1, static_cast<int>(std::lround(seconds / FrameTime)));
  }

  Vector3 Noise(float amount) {
    if (amount <= 0) {
      return {};
    }
    const float x = mNoise(mRandom);
    const float y = mNoise(mRandom);
    const float z = mNoise(mRandom);
    return Vector3 {x, y, z} * amount;
  }

  void Record(
    const TypingResult& result,
    const std::array<HandJoints, HandCount>& hands) {
    ++mFrames;
    ++gFrames;
    const bool same = result.mPanelID == mLast.mPanelID
      && result.mFingers == mLast.mFingers
      && result.mHandNearKeyboard == mLast.mHandNearKeyboard;
    if (same == result.mChanged) {
      ++gChangedMismatches;
    }
    if (!result.mChanged) {
      ++mUnchangedFrames;
    }
    for (std::size_t i = 0; i < HandCount; ++i) {
      auto& record = mFingers[i];
      const auto& finger = result.mFingers[i];
      const float tipDistance = (hands[i].mIsTracked && mKeyboard)
        ? ProjectOntoPanel(
            hands[i][Joint::IndexTip].mPosition, *mKeyboard, Viewer)
            .mDistance
        : std::numeric_limits<float>::quiet_NaN();
      const bool wasPressed = record.mLast.mState == TypingFingerState::Pressed;
      const bool isPressed = finger.mState == TypingFingerState::Pressed;
      const bool newPress = isPressed
        && ((!wasPressed) || finger.mPressCount != record.mLast.mPressCount);
      if (wasPressed && ((!isPressed) || newPress)) {
        ++record.mReleases;
        record.mReleaseTipDistances.push_back(tipDistance);
      }
      if (newPress) {
        ++record.mPresses;
        record.mPressDowns.push_back(finger);
        record.mPressTipDistances.push_back(tipDistance);
      } else if (
        isPressed
        && (finger.mU != record.mLast.mU || finger.mV != record.mLast.mV)) {
        record.mPressMoved = true;
      }
      record.mCountedPresses
        += static_cast<uint8_t>(finger.mPressCount - record.mLast.mPressCount);
      record.mHovered |= (finger.mState == TypingFingerState::Hovering);
      record.mFramesPressed += isPressed ? 1 : 0;
      record.mWasNear |= result.mHandNearKeyboard[i];
      record.mLast = finger;
    }
    mLast = result;
  }
};

// The usual way to type a key: hover 3cm in front, then a quick press, about
// 8mm through the keyboard
void Tap(Simulation& sim, float x, float y, float noise = 0) {
  sim.Hold(Pose::Point, PokeBasis, OnKeyboard(x, y, 0.03f), 0.3, noise);
  sim.Move(Pose::Point, PokeBasis, OnKeyboard(x, y, 0.03f), OnKeyboard(x, y, -0.008f), 0.11, noise);
  sim.Hold(Pose::Point, PokeBasis, OnKeyboard(x, y, -0.008f), 0.05, noise);
  sim.Move(Pose::Point, PokeBasis, OnKeyboard(x, y, -0.008f), OnKeyboard(x, y, 0.03f), 0.15, noise);
}

std::string PressCount(const FingerRecord& r) {
  return std::format("{} down, {} up", r.mPresses, r.mReleases);
}

}// namespace

int main() {
  Scenario("A straight press types one key, where the finger is", [] {
    Simulation sim;
    const float x = 0.05f;
    const float y = 0.02f;
    Tap(sim, x, y);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && r.mReleases == 1, std::format("one press ({})", PressCount(r)));
    Check(r.mCountedPresses == r.mPresses, "mPressCount counted it");
    Check(r.mHovered, "the finger hovered first");
    Check(
      !r.mPressDowns.empty() && MissMM(r.mPressDowns[0], x, y) < 1.0f,
      r.mPressDowns.empty() ? "landed on the key (no press)" : std::format("landed {:.2f}mm from the finger (limit 1mm)", MissMM(r.mPressDowns[0], x, y)));
    Check(
      !r.mPressTipDistances.empty() && r.mPressTipDistances[0] > 0 && r.mPressTipDistances[0] <= 0.006f,
      r.mPressTipDistances.empty() ? "pressed (no press)" : std::format("pressed with the fingertip {:.1f}mm in front of the keyboard", r.mPressTipDistances[0] * 1000));
    Check(
      !r.mReleaseTipDistances.empty() && r.mReleaseTipDistances[0] < 0,
      r.mReleaseTipDistances.empty() ? "released (no release)" : std::format("released as soon as it pulled back, still {:.1f}mm through", -r.mReleaseTipDistances[0] * 1000));
    Check(!r.mPressMoved, "the key didn't move while pressed");
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(x, y, 0.03f), OnKeyboard(x, y, 0.15f), 0.3);
    Check(sim.mLast.mFingers[1].mState == TypingFingerState::NotNear, "not near once the hand moved away");
    Check(!sim.mLast.mHandNearKeyboard[1], "... and the hand isn't near the keyboard");
    Check(sim.Left().mPresses == 0 && !sim.Left().mHovered, "the untracked left hand did nothing");
  });

  Scenario("Hovering close without crossing the touch plane never presses", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.02f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.02f), OnKeyboard(-0.1f, 0.03f, 0.01f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(-0.1f, 0.03f, 0.01f), OnKeyboard(0.1f, -0.03f, 0.01f), 1.0);
    const auto& r = sim.Right();
    Check(r.mPresses == 0, std::format("no presses (got {})", r.mPresses));
    Check(r.mHovered && sim.mLast.mFingers[1].mState == TypingFingerState::Hovering, "hovering");
    Check(sim.mLast.mHandNearKeyboard[1], "the hand is near the keyboard");
    const auto& last = sim.mLast.mFingers[1];
    Check(
      MissMM(last, 0.1f, -0.03f) < 3.0f,
      std::format("the hover position follows the finger moving at 21cm/s ({:.1f}mm behind, limit 3mm)", MissMM(last, 0.1f, -0.03f)));
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0.1f, -0.03f, 0.01f), 0.3);
    Check(
      MissMM(sim.mLast.mFingers[1], 0.1f, -0.03f) < 0.1f,
      std::format("... and catches up when it stops ({:.2f}mm away)", MissMM(sim.mLast.mFingers[1], 0.1f, -0.03f)));
    Check(std::abs(last.mDistance - 0.01f) < 0.001f, std::format("distance is reported ({:.1f}mm)", last.mDistance * 1000));
  });

  Scenario("A quick double tap, staying in the touch zone, types two letters", [] {
    Simulation sim;
    // Lifting 1cm between taps, from 6mm through to 4mm in front: never back
    // out past the 6mm touch plane
    const auto at = [](float d) { return OnKeyboard(0.02f, -0.01f, d); };
    sim.Hold(Pose::Point, PokeBasis, at(0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, at(0.03f), at(-0.006f), 0.1);
    sim.Move(Pose::Point, PokeBasis, at(-0.006f), at(0.004f), 0.1);
    sim.Move(Pose::Point, PokeBasis, at(0.004f), at(-0.006f), 0.1);
    sim.Move(Pose::Point, PokeBasis, at(-0.006f), at(0.03f), 0.15);
    const auto& r = sim.Right();
    Check(r.mPresses == 2 && r.mReleases == 2, std::format("two presses ({})", PressCount(r)));
    Check(r.mCountedPresses == r.mPresses, "mPressCount counted both");
    float worst = 0;
    for (const auto& down: r.mPressDowns) {
      worst = std::max(worst, MissMM(down, 0.02f, -0.01f));
    }
    Check(worst < 1.5f, std::format("both on the key (furthest {:.2f}mm, limit 1.5mm)", worst));
  });

  Scenario("A fast triple tap ('www') with 8mm lifts types three letters", [] {
    Simulation sim;
    const auto at = [](float d) { return OnKeyboard(-0.04f, 0.02f, d); };
    sim.Hold(Pose::Point, PokeBasis, at(0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, at(0.03f), at(-0.005f), 0.08);
    for (int i = 0; i < 2; ++i) {
      sim.Move(Pose::Point, PokeBasis, at(-0.005f), at(0.003f), 0.07);
      sim.Move(Pose::Point, PokeBasis, at(0.003f), at(-0.005f), 0.07);
    }
    sim.Move(Pose::Point, PokeBasis, at(-0.005f), at(0.03f), 0.1);
    const auto& r = sim.Right();
    Check(r.mPresses == 3 && r.mReleases == 3, std::format("three presses ({})", PressCount(r)));
  });

  Scenario("A finger drifting slowly onto the keyboard doesn't type", [] {
    Simulation sim;
    // 1.2cm/s
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.005f), 3.0);
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.005f), 1.5);
    Check(sim.Right().mPresses == 0, std::format("no presses (got {})", sim.Right().mPresses));
    // ... but pulling back out and tapping does
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.005f), OnKeyboard(0, 0, 0.03f), 0.3);
    Tap(sim, 0, 0);
    Check(sim.Right().mPresses == 1, std::format("a tap afterwards types (got {})", sim.Right().mPresses));

    int presses = 0;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
      Simulation noisy({}, seed);
      noisy.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3, 0.001f);
      noisy.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.005f), 3.0, 0.001f);
      noisy.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.005f), 1.5, 0.001f);
      presses += noisy.Right().mPresses;
    }
    Check(presses == 0, std::format("with +/-1mm of noise, no presses in 20 runs (got {})", presses));
  });

  Scenario("Hovering just above the touch plane, then tapping, types", [] {
    Simulation sim;
    // 2mm above the 6mm touch plane
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, 0.008f), 0.5);
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.008f), 0.5);
    Check(sim.Right().mPresses == 0, "no press while hovering");
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.008f), OnKeyboard(0, 0, -0.008f), 0.06);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), OnKeyboard(0, 0, 0.03f), 0.15);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && r.mReleases == 1, std::format("one press ({})", PressCount(r)));
    Check(!r.mPressDowns.empty() && MissMM(r.mPressDowns[0], 0, 0) < 1.0f, "on the key");
  });

  Scenario("A finger coming from behind the keyboard doesn't type", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.03f), OnKeyboard(0, 0, 0.03f), 0.2);
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.2);
    Check(sim.Right().mPresses == 0, std::format("no presses (got {})", sim.Right().mPresses));
    Tap(sim, 0, 0);
    Check(sim.Right().mPresses == 1, std::format("a tap afterwards types (got {})", sim.Right().mPresses));
  });

  Scenario("Reaching through the keyboard releases the key, and doesn't type on the way back", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.10f), 0.4);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && r.mReleases == 1, std::format("pressed, then released when reaching through ({})", PressCount(r)));
    Check(sim.mLast.mFingers[1].mState == TypingFingerState::NotNear, "not near 10cm behind the keyboard");
    Check(!sim.mLast.mHandNearKeyboard[1], "... and the hand is free to use other panels");
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.10f), OnKeyboard(0, 0, 0.03f), 0.4);
    Check(r.mPresses == 1, std::format("no press on the way back (got {})", r.mPresses));
  });

  Scenario("A finger sliding in from the side, already in the touch zone, doesn't type", [] {
    Simulation sim;
    // 3mm in front of the keyboard's plane, 10cm past its right edge
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0.25f, 0, 0.003f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0.25f, 0, 0.003f), OnKeyboard(0.05f, 0, 0.003f), 0.4);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0.05f, 0, 0.003f), OnKeyboard(0.05f, 0, -0.005f), 0.1);
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0.05f, 0, -0.005f), 0.2);
    Check(sim.Right().mPresses == 0, std::format("no presses (got {})", sim.Right().mPresses));
  });

  // Coming in at an angle, moving down the keyboard, meeting it at its center
  const auto angledPress = [](const TypingSettings& settings, float degrees, bool sideways) {
    Simulation sim(settings);
    const float slope = std::tan(degrees * std::numbers::pi_v<float> / 180);
    const auto at = [&](float d) {
      return sideways ? OnKeyboard(-slope * d, 0, d) : OnKeyboard(0, slope * d, d);
    };
    // About 30cm/s along the way the finger moves
    const double seconds = 0.05 * std::sqrt(1 + (slope * slope)) / 0.3;
    sim.Hold(Pose::Point, PokeBasis, at(0.04f), 0.3);
    sim.Move(Pose::Point, PokeBasis, at(0.04f), at(-0.01f), seconds);
    sim.Move(Pose::Point, PokeBasis, at(-0.01f), at(0.04f), 0.2);
    return sim.Right();
  };
  for (const auto& [degrees, sideways]: {std::pair {30.0f, false}, std::pair {45.0f, false}, std::pair {30.0f, true}}) {
    Scenario(
      std::format(
        "Coming in at {:.0f} degrees{}, the press lands where the finger is heading",
        degrees,
        sideways ? " from the side" : "")
        .c_str(),
      [&] {
        TypingSettings straightOnto;
        straightOnto.mMaxApproachSlope = 0;
        const auto before = angledPress(straightOnto, degrees, sideways);
        const auto r = angledPress({}, degrees, sideways);
        Check(r.mPresses == 1 && r.mReleases == 1, std::format("one press ({})", PressCount(r)));
        const float miss = r.mPressDowns.empty() ? 1000.0f : MissMM(r.mPressDowns[0], 0, 0);
        const float missBefore = before.mPressDowns.empty() ? 1000.0f : MissMM(before.mPressDowns[0], 0, 0);
        Check(
          miss < 3.0f,
          std::format(
            "lands {:.1f}mm from where the finger meets the keyboard (limit 3mm; straight onto the keyboard it would be {:.1f}mm)",
            miss,
            missBefore));
      });
  }

  Scenario("Aim quickly, then press at once: the key is under the finger", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(-0.04f, 0, 0.03f), 0.3);
    // A quick 6cm aiming move (30cm/s), then straight into a press
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(-0.04f, 0, 0.03f), OnKeyboard(0.02f, 0, 0.03f), 0.2);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0.02f, 0, 0.03f), OnKeyboard(0.02f, 0, -0.008f), 0.1);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0.02f, 0, -0.008f), OnKeyboard(0.02f, 0, 0.03f), 0.15);
    const auto& r = sim.Right();
    Check(r.mPresses == 1, std::format("one press (got {})", r.mPresses));
    const float miss = r.mPressDowns.empty() ? 1000.0f : MissMM(r.mPressDowns[0], 0.02f, 0);
    Check(miss < 3.0f, std::format("lands {:.1f}mm from the finger (limit 3mm)", miss));
  });

  Scenario("A tilted, desk-like keyboard, pressed from above", [] {
    // Tilted back 60 degrees from upright; the finger points and moves
    // straight down, 30 degrees from the keyboard's normal
    Panel desk = Keyboard;
    desk.mPosition = {0, 0.9f, -0.4f};
    desk.mOrientation = Quaternion::CreateFromAxisAngle(Vector3::UnitX, -std::numbers::pi_v<float> / 3);
    const auto right = Vector3::Transform(Vector3::UnitX, desk.mOrientation);
    const auto up = Vector3::Transform(Vector3::UnitY, desk.mOrientation);
    const Vector3 target = desk.mPosition + (right * 0.06f) + (up * -0.02f);
    const auto expected = ProjectOntoPanel(target, desk, Viewer);
    const Vector3 above = target + Vector3 {0, 0.05f, 0};
    const Vector3 below = target - Vector3 {0, 0.01f, 0};

    Simulation sim({}, 1, desk);
    sim.Hold(Pose::Point, DownBasis, above, 0.3);
    sim.Move(Pose::Point, DownBasis, above, below, 0.2);
    sim.Move(Pose::Point, DownBasis, below, above, 0.2);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && r.mReleases == 1, std::format("one press ({})", PressCount(r)));
    const float miss = r.mPressDowns.empty()
      ? 1000.0f
      : std::hypot((r.mPressDowns[0].mU - expected.mU) * KeyboardWidth, (r.mPressDowns[0].mV - expected.mV) * KeyboardHeight) * 1000;
    Check(miss < 2.0f, std::format("lands {:.1f}mm from where the finger meets the keyboard (limit 2mm)", miss));
  });

  Scenario("Both hands pressing at the same moment: one press each", [] {
    Simulation sim;
    const auto leftOut = OnKeyboard(-0.08f, 0, 0.03f);
    const auto leftIn = OnKeyboard(-0.08f, 0, -0.008f);
    const auto rightOut = OnKeyboard(0.08f, 0.02f, 0.03f);
    const auto rightIn = OnKeyboard(0.08f, 0.02f, -0.008f);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightOut, rightOut, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftIn, rightOut, rightIn, 0.1);
    sim.MoveBoth(Pose::Point, leftIn, leftIn, rightIn, rightIn, 0.05);
    sim.MoveBoth(Pose::Point, leftIn, leftOut, rightIn, rightOut, 0.15);
    const auto& left = sim.Left();
    const auto& right = sim.Right();
    Check(left.mPresses == 1 && left.mReleases == 1, std::format("left: one press ({})", PressCount(left)));
    Check(right.mPresses == 1 && right.mReleases == 1, std::format("right: one press ({})", PressCount(right)));
    Check(!left.mPressDowns.empty() && MissMM(left.mPressDowns[0], -0.08f, 0) < 1.0f, "left pressed its own key");
    Check(!right.mPressDowns.empty() && MissMM(right.mPressDowns[0], 0.08f, 0.02f) < 1.0f, "right pressed its own key");
  });

  Scenario("One hand holds a key while the other types twice", [] {
    Simulation sim;
    const auto leftOut = OnKeyboard(-0.08f, 0, 0.03f);
    const auto leftIn = OnKeyboard(-0.08f, 0, -0.008f);
    const auto rightOut = OnKeyboard(0.08f, 0, 0.03f);
    const auto rightIn = OnKeyboard(0.08f, 0, -0.008f);
    sim.MoveBoth(Pose::Point, leftOut, leftOut, rightOut, rightOut, 0.3);
    sim.MoveBoth(Pose::Point, leftOut, leftIn, rightOut, rightOut, 0.1);
    for (int i = 0; i < 2; ++i) {
      sim.MoveBoth(Pose::Point, leftIn, leftIn, rightOut, rightIn, 0.1);
      sim.MoveBoth(Pose::Point, leftIn, leftIn, rightIn, rightOut, 0.12);
    }
    const auto& left = sim.Left();
    const auto& right = sim.Right();
    Check(left.mPresses == 1 && left.mReleases == 0, std::format("left: held ({})", PressCount(left)));
    Check(right.mPresses == 2 && right.mReleases == 2, std::format("right: two presses ({})", PressCount(right)));
  });

  Scenario("A fist pushed into the keyboard doesn't type", [] {
    Simulation sim;
    sim.Hold(Pose::Fist, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Fist, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.02f), 0.15);
    sim.Move(Pose::Fist, PokeBasis, OnKeyboard(0, 0, -0.02f), OnKeyboard(0, 0, 0.03f), 0.15);
    const auto& r = sim.Right();
    Check(r.mPresses == 0, std::format("no presses (got {})", r.mPresses));
    Check(!r.mHovered, "not even hovering");
    Check(r.mWasNear, "the hand still counts as near the keyboard");
  });

  Scenario("A hand resting flat on the keyboard doesn't type", [] {
    Simulation sim;
    // Palm toward the keyboard, coming down quickly (30cm/s), then resting
    sim.Hold(Pose::Flat, PalmOnBasis, OnKeyboard(0, -0.03f, 0.04f), 0.3);
    sim.Move(Pose::Flat, PalmOnBasis, OnKeyboard(0, -0.03f, 0.04f), OnKeyboard(0, -0.03f, -0.005f), 0.15);
    sim.Hold(Pose::Flat, PalmOnBasis, OnKeyboard(0, -0.03f, -0.005f), 0.5);
    Check(sim.Right().mPresses == 0, std::format("palm down: no presses (got {})", sim.Right().mPresses));
    Check(sim.Diagnostics(Hand::Right).mIsResting, std::format("... it's resting ({} fingertips)", sim.Diagnostics(Hand::Right).mRestingFingertips));
    Check(sim.Right().mWasNear, "the hand still counts as near the keyboard");
    sim.Move(Pose::Flat, PalmOnBasis, OnKeyboard(0, -0.03f, -0.005f), OnKeyboard(0, -0.03f, 0.04f), 0.2);

    // Flat hand, fingers first: the middle finger is longer, and gets there
    // first
    Simulation fingersFirst;
    fingersFirst.Hold(Pose::Flat, PokeBasis, OnKeyboard(0, 0, 0.04f), 0.3);
    fingersFirst.Move(Pose::Flat, PokeBasis, OnKeyboard(0, 0, 0.04f), OnKeyboard(0, 0, -0.005f), 0.15);
    fingersFirst.Hold(Pose::Flat, PokeBasis, OnKeyboard(0, 0, -0.005f), 0.3);
    fingersFirst.Move(Pose::Flat, PokeBasis, OnKeyboard(0, 0, -0.005f), OnKeyboard(0, 0, 0.04f), 0.2);
    Check(fingersFirst.Right().mPresses == 0, std::format("fingers first: no presses (got {})", fingersFirst.Right().mPresses));

    // Pointing again afterwards types normally
    Tap(sim, 0, 0);
    Check(sim.Right().mPresses == 1, std::format("pointing afterwards types (got {})", sim.Right().mPresses));
  });

  Scenario("Jitter (+/-2mm) while hovering 1cm away for 3s doesn't type", [] {
    int presses = 0;
    int hovering = 0;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
      Simulation sim({}, seed);
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3, 0.002f);
      sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, 0.01f), 0.3, 0.002f);
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.01f), 3.0, 0.002f);
      presses += sim.Right().mPresses;
      hovering += (sim.mLast.mFingers[1].mState == TypingFingerState::Hovering);
    }
    Check(presses == 0, std::format("no presses in 20 runs (got {})", presses));
    Check(hovering == 20, std::format("still hovering at the end in {} of 20 runs", hovering));
  });

  Scenario("Jitter (+/-1.5mm) while hovering 1mm above the touch plane for 3s doesn't type", [] {
    int presses = 0;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
      Simulation sim({}, seed);
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3, 0.0015f);
      sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, 0.007f), 0.5, 0.0015f);
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.007f), 3.0, 0.0015f);
      presses += sim.Right().mPresses;
    }
    Check(presses == 0, std::format("no presses in 20 runs (got {})", presses));
  });

  Scenario("Noisy presses (+/-2mm): exactly one key each, in 100 runs", [] {
    int exact = 0;
    int extra = 0;
    int missed = 0;
    float worst = 0;
    float total = 0;
    for (uint32_t seed = 1; seed <= 100; ++seed) {
      Simulation sim({}, seed);
      const float x = -0.1f + (0.002f * static_cast<float>(seed));
      const float y = 0.03f - (0.0006f * static_cast<float>(seed));
      Tap(sim, x, y, 0.002f);
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(x, y, 0.03f), 0.3, 0.002f);
      const auto& r = sim.Right();
      exact += (r.mPresses == 1 && r.mReleases == 1);
      extra += std::max(0, r.mPresses - 1);
      missed += (r.mPresses == 0);
      for (const auto& down: r.mPressDowns) {
        const float miss = MissMM(down, x, y);
        worst = std::max(worst, miss);
        total += miss;
      }
    }
    Check(exact == 100, std::format("one press and release in {} of 100 runs ({} extra, {} missed)", exact, extra, missed));
    Check(
      worst < 4.0f,
      std::format("presses land {:.1f}mm from the finger on average, {:.1f}mm at worst (limit 4mm)", total / 100, worst));
  });

  for (const float noise: {0.001f, 0.0015f, 0.002f}) {
    Scenario(
      std::format("Noisy (+/-{:.1f}mm) double taps with 1cm lifts: two letters each, in 50 runs", noise * 1000).c_str(),
      [noise] {
        int exact = 0;
        int extra = 0;
        for (uint32_t seed = 1; seed <= 50; ++seed) {
          Simulation sim({}, seed);
          const auto at = [](float d) { return OnKeyboard(0.02f, -0.01f, d); };
          sim.Hold(Pose::Point, PokeBasis, at(0.03f), 0.4, noise);
          sim.Move(Pose::Point, PokeBasis, at(0.03f), at(-0.006f), 0.1, noise);
          sim.Move(Pose::Point, PokeBasis, at(-0.006f), at(0.004f), 0.1, noise);
          sim.Move(Pose::Point, PokeBasis, at(0.004f), at(-0.006f), 0.1, noise);
          sim.Move(Pose::Point, PokeBasis, at(-0.006f), at(0.03f), 0.15, noise);
          exact += (sim.Right().mPresses == 2 && sim.Right().mReleases == 2);
          extra += std::max(0, sim.Right().mPresses - 2);
        }
        Check(exact == 50, std::format("two presses in {} of 50 runs ({} extra)", exact, extra));
      });
  }

  Scenario("Losing tracking mid-press releases, with no extra press when it's back", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.008f), 0.11);
    Check(sim.mLast.mFingers[1].mState == TypingFingerState::Pressed, "pressed");
    sim.LoseTracking(0.2);
    const auto& r = sim.Right();
    Check(r.mReleases == 1 && sim.mLast.mFingers[1].mState == TypingFingerState::NotNear, "released, and not near, while tracking is lost");
    Check(!sim.mLast.mHandNearKeyboard[1], "an untracked hand isn't near the keyboard");
    // Back where it was, then pulls out
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), OnKeyboard(0, 0, 0.03f), 0.15);
    Check(r.mPresses == 1, std::format("no extra press when tracking came back (got {})", r.mPresses));
    // Coming back already moving toward the keyboard, just in front of it
    sim.LoseTracking(0.2);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.012f), OnKeyboard(0, 0, -0.008f), 0.06);
    Check(r.mPresses == 1, std::format("... nor when it comes back moving in (got {})", r.mPresses));
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), OnKeyboard(0, 0, 0.03f), 0.15);
    Tap(sim, 0, 0);
    Check(r.mPresses == 2, std::format("then a tap types (got {})", r.mPresses));
  });

  Scenario("A long gap between frames releases, with no extra press afterwards", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.008f), 0.11);
    sim.Pause(0.5);
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), 1.0 / 90);
    const auto& r = sim.Right();
    Check(r.mReleases == 1 && sim.mLast.mFingers[1].mState == TypingFingerState::NotNear, "released (not near) on the first frame after the gap");
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), OnKeyboard(0, 0, 0.03f), 0.15);
    Check(r.mPresses == 1, std::format("no extra press (got {})", r.mPresses));
  });

  Scenario("Press and hold for a second: one press, held in place", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.008f), 0.11);
    const int before = sim.Right().mFramesPressed;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), 1.0);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && r.mReleases == 0, std::format("held ({})", PressCount(r)));
    Check(r.mFramesPressed - before == 90, std::format("pressed for all 90 frames (got {})", r.mFramesPressed - before));
    Check(!r.mPressMoved, "the key didn't move");

    // Holding with tracking noise, drifting 5mm sideways
    Simulation noisy;
    noisy.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3, 0.001f);
    noisy.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.008f), 0.11, 0.001f);
    noisy.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), OnKeyboard(0.005f, 0, -0.008f), 1.0, 0.001f);
    const auto& n = noisy.Right();
    Check(n.mPresses == 1 && n.mReleases == 0 && !n.mPressMoved, std::format("with +/-1mm of noise and drifting sideways: still one press, in place ({})", PressCount(n)));
  });

  Scenario("A held key is released by pulling back, even slowly", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.01f), 0.11);
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.01f), 0.6);
    Check(sim.Diagnostics(Hand::Right).mIsHold, "it's a hold");
    // Easing off by 4mm doesn't let go of a hold
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.01f), OnKeyboard(0, 0, -0.006f), 0.3);
    Check(sim.Right().mReleases == 0, "easing off 4mm keeps holding");
    // 2cm/s
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.006f), OnKeyboard(0, 0, 0.03f), 1.8);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && r.mReleases == 1, std::format("one press ({})", PressCount(r)));
    Check(
      !r.mReleaseTipDistances.empty() && r.mReleaseTipDistances[0] < 0.012f,
      r.mReleaseTipDistances.empty() ? "released (no release)" : std::format("released with the fingertip {:.1f}mm in front (limit 12mm)", r.mReleaseTipDistances[0] * 1000));
  });

  Scenario("A key press doesn't drag", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.008f), 0.11);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), OnKeyboard(0.03f, 0, -0.008f), 0.3);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && !r.mPressMoved, "the key stayed where it was pressed");
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0.03f, 0, -0.008f), OnKeyboard(0.03f, 0, 0.03f), 0.15);
    Check(r.mPresses == 1 && r.mReleases == 1, std::format("one press ({})", PressCount(r)));
  });

  Scenario("Broken tracking data (NaN) counts as not tracked", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    auto broken = MakeHand(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.01f));
    broken[Joint::IndexTip].mPosition.z = std::numeric_limits<float>::quiet_NaN();
    broken[Joint::Wrist].mRadius = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 5; ++i) {
      sim.Step(HandJoints {}, broken);
    }
    Check(sim.mLast.mFingers[1].mState == TypingFingerState::NotNear, "not near");
    Tap(sim, 0, 0);
    const auto& r = sim.Right();
    Check(r.mPresses == 1 && r.mReleases == 1, std::format("a tap afterwards types ({})", PressCount(r)));
    Check(!r.mPressDowns.empty() && MissMM(r.mPressDowns[0], 0, 0) < 1.0f, "on the key");
  });

  Scenario("Reset() releases a held key", [] {
    Simulation sim;
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.008f), 0.11);
    sim.Reset();
    Check(
      sim.mLast.mChanged && sim.mLast.mFingers[1].mState == TypingFingerState::NotNear && sim.mLast.mPanelID == 0,
      "released");
    Check(sim.mLast.mFingers[1].mPressCount == 1, "the press count is kept");
    sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.008f), 0.3);
    Check(sim.Right().mPresses == 1, "no press afterwards while still in the keyboard");
  });

  Scenario("Without a keyboard, nothing happens", [] {
    Simulation sim({}, 1, std::nullopt);
    Tap(sim, 0, 0);
    Check(sim.Right().mPresses == 0 && !sim.Right().mHovered && !sim.Right().mWasNear, "nothing");
    Check(sim.mLast.mPanelID == 0, "no panel");
  });

  Scenario("Nothing changes while the hands are away from the keyboard", [] {
    Simulation sim;
    sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.3f), OnKeyboard(0.1f, 0.05f, 0.25f), 0.5);
    Check(sim.mUnchangedFrames == sim.mFrames - 1, std::format("only the first frame changed ({} of {} unchanged)", sim.mUnchangedFrames, sim.mFrames));
  });

  std::printf("\nTuning summary\n");
  {
    // Time from the real fingertip crossing the touch plane to the press,
    // pressing in at `speed`
    const auto pressDelay = [](float speed) {
      Simulation sim;
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
      std::optional<int> crossedAt;
      int frame = 0;
      for (; frame < 200 && sim.Right().mPresses == 0; ++frame) {
        const float d = 0.03f - (speed * static_cast<float>(frame + 1) / 90);
        sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, d), 1.0 / 90);
        if (!crossedAt && d <= TypingSettings {}.mTouchDistance) {
          crossedAt = frame;
        }
      }
      return (frame - 1 - crossedAt.value_or(frame - 1)) * 1000.0f / 90;
    };
    // Time from the fingertip starting to pull back (at 25cm/s) to the
    // release
    const auto releaseDelay = []() {
      Simulation sim;
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 0.3);
      sim.Move(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), OnKeyboard(0, 0, -0.01f), 0.15);
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, -0.01f), 0.05);
      int frame = 0;
      for (; frame < 200 && sim.Right().mReleases == 0; ++frame) {
        const float d = -0.01f + (0.25f * static_cast<float>(frame + 1) / 90);
        sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, d), 1.0 / 90);
      }
      return (frame - 1) * 1000.0f / 90;
    };
    // Noise estimate and release distance after hovering with noise
    const auto noiseAt = [](float noise) {
      Simulation sim;
      sim.Hold(Pose::Point, PokeBasis, OnKeyboard(0, 0, 0.03f), 1.0, noise);
      return sim.Diagnostics(Hand::Right);
    };
    std::printf("  press starts %.0fms after the fingertip reaches the touch plane at 10cm/s, %.0fms at 25cm/s\n", pressDelay(0.10f), pressDelay(0.25f));
    std::printf("  release %.0fms after the fingertip starts pulling back at 25cm/s\n", releaseDelay());
    for (const float noise: {0.0f, 0.001f, 0.002f, 0.003f}) {
      const auto diag = noiseAt(noise);
      std::printf(
        "  +/-%.0fmm noise: estimated %.2fmm; release/re-press distance %.1fmm\n",
        noise * 1000,
        diag.mTrackingNoise * 1000,
        diag.mRetractDistance * 1000);
    }
    Check(pressDelay(0.25f) < 12.0f, "a press starts within a frame of the fingertip reaching the touch plane at 25cm/s");
  }

  Check(gChangedMismatches == 0, std::format("mChanged matched whether anything changed, in all {} frames ({} mismatches)", gFrames, gChangedMismatches));

  std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
