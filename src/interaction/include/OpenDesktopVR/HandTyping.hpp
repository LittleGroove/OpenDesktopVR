// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Pure logic for typing on a virtual keyboard panel with the index fingertips.
// Unlike `Tracker`, there's no pose to hold: the fingertip simply presses keys
// as it pushes into the panel, and both hands type independently. No OpenXR
// or graphics dependencies, so it can be tested with synthetic hands.
#pragma once

#include <OpenDesktopVR/HandInteraction.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace OpenDesktopVR::HandInteraction {

struct TypingSettings {
  // Distances are in meters, times are in seconds, speeds in meters/second

  // A fingertip hovers over a key when it is this close in front of the
  // keyboard...
  float mHoverDistance {0.04f};
  // ... and over it, or no further than this outside its edges
  float mEdgeMargin {0.005f};
  // Once hovering, it can move this much further away (in both directions)
  // before it stops, so it doesn't flicker at the edges of the hover zone
  float mHoverHysteresis {0.01f};
  // Fingertips further behind the keyboard than this are reaching through it,
  // and are ignored (releasing any key)
  float mMaxPushThrough {0.06f};

  // A key press starts when the fingertip crosses this 'touch plane', in front
  // of the surface; this makes up for the fingertip's radius, and for the
  // finger stopping late without anything to push against
  float mTouchDistance {0.006f};
  // ... while moving toward the keyboard at least this fast; slower is
  // resting or drifting, not typing...
  float mMinApproachSpeed {0.07f};
  // ... measured over this long
  float mVelocityWindow {0.04f};
  // ... having moved toward the keyboard by at least the stroke distance (the
  // same as the release distance below) within this long, so that tracking
  // jitter right at the touch plane can't press keys
  float mStrokeWindow {0.15f};

  // A press lands where the fingertip is heading as it crosses the touch
  // plane (not straight under it), up to this angle from straight in, as a
  // slope (1 = 45 degrees)
  float mMaxApproachSlope {1.0f};

  // A press ends when the fingertip pulls back from the deepest point it
  // reached by at least this much...
  float mMinRetractDistance {0.002f};
  // ... or this many times the measured tracking noise, if that's more, so
  // that jitter can't end a press early (or start another one)...
  float mRetractNoiseMultiplier {5.5f};
  // ... but never more than this. A new press can then start without leaving
  // the touch plane, as soon as the fingertip moves toward the keyboard again
  // by the same distance (double letters).
  float mMaxRetractDistance {0.006f};
  // Presses held longer than this are holds (e.g. for key repeat). Hand
  // tracking wobbles slowly as well as jittering, so holds end only when the
  // fingertip is pulled back this far from how deep it's held...
  float mLongPressTime {0.30f};
  float mHoldRetractDistance {0.008f};
  // ... which follows the fingertip drifting back out, with this time
  // constant. Holds also end when the fingertip is back out in front of the
  // touch plane (by `mRearmDistance`).
  float mHoldRelaxTime {0.25f};

  // When a fingertip in the touch zone can't press (see `Blocked`), it must
  // come back out this much further than `mTouchDistance` first
  float mRearmDistance {0.003f};
  // After a press ends, the fingertip can press again from inside the touch
  // zone for this long; after that, it must come back out first
  float mRepressTime {0.6f};
  // A fingertip that crossed into the touch zone too slowly can still press
  // for this long, if it speeds up (e.g. hovering just above the touch plane,
  // then pressing)
  float mSlowEntryTime {0.25f};
  // Per finger: the shortest time from the end of one press to the start of
  // the next
  float mPressCooldown {0.08f};

  // Hands that are ignored: the index finger curled into a fist (curl: 0 =
  // straight, 1 = fully curled; the lower value ends it, for hysteresis)...
  float mFistIndexCurl {0.70f};
  float mFistIndexReleaseCurl {0.60f};
  // ... or the hand resting on the keyboard: at least this many fingertips
  // (thumb included) closer than `mRestingDistance` to the keyboard surface,
  // and over it (within `mRestingEdgeMargin` of its edges). That's slightly in
  // front of the touch plane, so a flat hand coming down is caught before its
  // index fingertip presses a key.
  uint8_t mRestingFingertips {3};
  float mRestingDistance {0.012f};
  float mRestingEdgeMargin {0.02f};

  // If there's no update for this long, every key is released
  float mMaxFrameGap {0.25f};
  // After tracking is lost, or the keyboard changes, presses can only start
  // once the hand has been tracked this long again, so a hand reappearing
  // in a different place doesn't press keys
  float mReacquireDelay {0.10f};

  // Moves presses and hovering up the keyboard by this much (negative =
  // down), to correct for a personal offset
  float mTouchOffsetUp {0.0f};

  // One Euro filter parameters; see `Settings`. 'Hover' is the hover
  // position, 'touch' is where presses land, and 'depth' decides when presses
  // start and end.
  float mHoverMinCutoff {5.0f};// Hz
  float mHoverBeta {20.0f};
  float mTouchMinCutoff {8.0f};// Hz
  float mTouchBeta {20.0f};
  float mDepthMinCutoff {15.0f};// Hz
  float mDepthBeta {30.0f};
  float mDerivativeCutoff {2.5f};// Hz
  // How quickly the tracking noise estimate follows changes
  float mNoiseTimeConstant {0.5f};
};

enum class TypingFingerState : uint8_t {
  NotNear = 0,
  Hovering = 1,
  Pressed = 2,
};

struct TypingFinger {
  TypingFingerState mState {TypingFingerState::NotNear};
  // 0..1 across the keyboard (0 = left/top); same convention as
  // `CursorUpdate`. While pressed, this is where the press landed, and doesn't
  // move. All 0 while not near.
  float mU {};
  float mV {};
  // Meters in front of the keyboard surface (negative = through it)
  float mDistance {};
  // Increments (wrapping) every time a new press starts; kept across Reset()
  uint8_t mPressCount {};

  bool operator==(const TypingFinger&) const = default;
};

struct TypingResult {
  // The keyboard panel, 0 if none
  uint64_t mPanelID {};
  // The index fingertip of each hand
  std::array<TypingFinger, HandCount> mFingers {};
  // The hand's index fingertip (any part of it) is within the hover zone,
  // even if the hand is ignored (fist, resting); don't let the pointing
  // `Tracker` click other panels with this hand
  std::array<bool, HandCount> mHandNearKeyboard {};
  // Anything above differs from the previous result
  bool mChanged {false};
};

// Internal state of each finger, for logs and tuning
enum class TypingPhase : uint8_t {
  // Not near the keyboard, not tracked, or the hand is ignored
  Idle,
  // In front of the keyboard, ready to press
  Hovering,
  Pressed,
  // In the touch zone, not pressed: a press just ended, or the fingertip
  // crossed in too slowly. It presses if it moves toward the keyboard again
  // soon (see `TypingSettings::mRepressTime` and `mSlowEntryTime`).
  InZone,
  // In the touch zone, but it didn't get there by approaching from the front
  // (from the side or behind, after tracking was lost, or after the hand
  // stopped resting); can't press until it comes back out
  Blocked,
};

struct TypingDiagnostics {
  bool mIsTracked {false};
  TypingPhase mPhase {TypingPhase::Idle};
  float mIndexCurl {};
  bool mIsFist {false};
  uint8_t mRestingFingertips {};
  bool mIsResting {false};
  // Estimated fingertip tracking noise (standard deviation), and the
  // pull-back that ends a quick press (also the stroke that starts one)
  float mTrackingNoise {};
  float mRetractDistance {};
  bool mIsHold {false};
  // Toward the keyboard, measured over `mVelocityWindow`
  float mApproachSpeed {};
  float mDistance {};
  float mEdgeDistance {};
};

class TypingTracker final {
 public:
  explicit TypingTracker(const TypingSettings& = {});

  // Call once per frame, with the latest hand joints (non-finite joint data
  // counts as not tracked). `keyboard` is the keyboard panel, if any is shown.
  [[nodiscard]]
  TypingResult Update(
    double nowSeconds,
    const std::array<HandJoints, HandCount>& hands,
    const std::optional<Panel>& keyboard,
    const Vector3& viewerPosition);

  // Releases everything; the next Update() starts afresh
  [[nodiscard]]
  TypingResult Reset();

  [[nodiscard]]
  const TypingSettings& GetSettings() const noexcept {
    return mSettings;
  }

  void SetTouchOffsetUp(float meters) noexcept {
    mSettings.mTouchOffsetUp = std::isfinite(meters) ? meters : 0.0f;
  }

  // As of the last Update()
  [[nodiscard]]
  const std::array<TypingDiagnostics, HandCount>& GetDiagnostics()
    const noexcept {
    return mDiagnostics;
  }

 private:
  struct Sample {
    double mTime {};
    // Depth-filtered fingertip
    Vector3 mTip {};
    // ... and its distance in front of the keyboard
    float mDistance {};
  };
  static constexpr std::size_t HistorySize = 48;

  struct HandState {
    TypingPhase mPhase {TypingPhase::Idle};

    OneEuroFilter mHoverFilter;
    OneEuroFilter mTouchFilter;
    OneEuroFilter mDepthFilter;

    // Tracking noise: from the last two depth-filtered fingertips
    std::array<Vector3, 2> mRecentDepthTips {};
    int mRecentDepthTipCount {0};
    float mTrackingNoise {0.0005f};

    // Recent depth-filtered fingertips (a ring buffer), for how fast and in
    // which direction the fingertip is moving, and how far it has moved
    std::array<Sample, HistorySize> mHistory {};
    std::size_t mHistoryNext {0};
    std::size_t mHistoryCount {0};

    // Since when the hand has been usable, without a break
    std::optional<double> mTrackedSince;
    // Last frame's lightly-smoothed fingertip and depth, for where exactly it
    // crossed the touch plane
    std::optional<Vector3> mPreviousTouchTip;
    float mPreviousDistance {};

    bool mIsFist {false};
    bool mIsNear {false};

    // The current press
    Vector2 mPressUV {};
    double mPressStartTime {};
    // The deepest the fingertip went; for holds, follows it drifting back out
    float mDeepest {};
    // When the last press ended
    std::optional<double> mReleaseTime;
    // InZone: presses may start until this time, from strokes that start no
    // earlier than `mInZoneSince` (if set)
    double mInZoneUntil {};
    std::optional<double> mInZoneSince;

    uint8_t mPressCount {0};
    TypingFinger mFinger {};
  };

  TypingSettings mSettings;
  std::array<HandState, HandCount> mHands {};
  std::array<TypingDiagnostics, HandCount> mDiagnostics {};
  std::optional<double> mLastUpdate;
  std::optional<uint64_t> mPanelID;
  TypingResult mLastResult {};

  // Releases, and forgets everything except the press count and noise level
  void ResetHand(HandState&, std::optional<double> now, bool keepNear);
  void ResetAll(std::optional<double> now, bool keepNear);

  void UpdateHand(
    HandState&,
    TypingDiagnostics&,
    double now,
    float dt,
    const HandJoints&,
    const Panel& keyboard,
    const Vector3& viewerPosition);

  [[nodiscard]]
  TypingResult MakeResult();
};

}// namespace OpenDesktopVR::HandInteraction
