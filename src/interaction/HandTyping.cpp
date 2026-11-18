// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).

#include <OpenDesktopVR/HandTyping.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace OpenDesktopVR::HandInteraction {

namespace {

// NaN or infinity from hand tracking would stick in the smoothing filters
bool IsUsable(const HandJoints& hand) {
  if (!hand.mIsTracked) {
    return false;
  }
  return std::ranges::all_of(hand.mJoints, [](const JointLocation& joint) {
    return std::isfinite(joint.mPosition.x) && std::isfinite(joint.mPosition.y)
      && std::isfinite(joint.mPosition.z) && std::isfinite(joint.mRadius);
  });
}

constexpr std::array FingerTips {
  Joint::ThumbTip,
  Joint::IndexTip,
  Joint::MiddleTip,
  Joint::RingTip,
  Joint::LittleTip,
};

constexpr double Forever = -std::numeric_limits<double>::infinity();

}// namespace

TypingTracker::TypingTracker(const TypingSettings& settings)
  : mSettings(settings) {
}

void TypingTracker::ResetHand(
  HandState& hand,
  std::optional<double> now,
  bool keepNear) {
  HandState fresh {};
  fresh.mPressCount = hand.mPressCount;
  fresh.mFinger.mPressCount = hand.mPressCount;
  // The noise level is kept on purpose (it changes slowly), but must not stay
  // stuck at NaN or infinity
  if (std::isfinite(hand.mTrackingNoise)) {
    fresh.mTrackingNoise = hand.mTrackingNoise;
  }
  fresh.mReleaseTime = hand.mReleaseTime;
  if (hand.mPhase == TypingPhase::Pressed && now) {
    fresh.mReleaseTime = now;
  }
  if (keepNear) {
    fresh.mIsNear = hand.mIsNear;
  }
  hand = fresh;
}

void TypingTracker::ResetAll(std::optional<double> now, bool keepNear) {
  for (std::size_t i = 0; i < HandCount; ++i) {
    ResetHand(mHands[i], now, keepNear);
    mDiagnostics[i] = {};
  }
}

TypingResult TypingTracker::Update(
  double nowSeconds,
  const std::array<HandJoints, HandCount>& hands,
  const std::optional<Panel>& keyboard,
  const Vector3& viewerPosition) {
  if (!keyboard) {
    ResetAll(nowSeconds, false);
    mPanelID.reset();
    mLastUpdate = nowSeconds;
    return MakeResult();
  }

  if (mPanelID != keyboard->mID) {
    // A different keyboard: start afresh
    ResetAll(nowSeconds, false);
    mPanelID = keyboard->mID;
  }

  float dt = 0;
  if (mLastUpdate) {
    const double elapsed = nowSeconds - *mLastUpdate;
    if (!(elapsed >= 0 && elapsed <= mSettings.mMaxFrameGap)) {
      // Frames stopped for a while (e.g. the game was paused), or time went
      // backwards: release everything, and start afresh next frame. Whether
      // the hands are near the keyboard is kept for this frame, so the
      // pointing tracker doesn't get them for just one frame.
      ResetAll(nowSeconds, true);
      mLastUpdate = nowSeconds;
      return MakeResult();
    }
    dt = static_cast<float>(elapsed);
  }
  mLastUpdate = nowSeconds;

  for (std::size_t i = 0; i < HandCount; ++i) {
    if (!IsUsable(hands[i])) {
      ResetHand(mHands[i], nowSeconds, false);
      mDiagnostics[i] = {};
      continue;
    }
    UpdateHand(
      mHands[i],
      mDiagnostics[i],
      nowSeconds,
      dt,
      hands[i],
      *keyboard,
      viewerPosition);
  }
  return MakeResult();
}

TypingResult TypingTracker::Reset() {
  ResetAll(mLastUpdate, false);
  mLastUpdate.reset();
  mPanelID.reset();
  return MakeResult();
}

TypingResult TypingTracker::MakeResult() {
  TypingResult result {.mPanelID = mPanelID.value_or(0)};
  for (std::size_t i = 0; i < HandCount; ++i) {
    result.mFingers[i] = mHands[i].mFinger;
    result.mHandNearKeyboard[i] = mHands[i].mIsNear;
  }
  result.mChanged = !(
    result.mPanelID == mLastResult.mPanelID
    && result.mFingers == mLastResult.mFingers
    && result.mHandNearKeyboard == mLastResult.mHandNearKeyboard);
  mLastResult = result;
  return result;
}

void TypingTracker::UpdateHand(
  HandState& hand,
  TypingDiagnostics& diag,
  double now,
  float dt,
  const HandJoints& joints,
  const Panel& keyboard,
  const Vector3& viewerPosition) {
  const auto& s = mSettings;
  if (!hand.mTrackedSince) {
    hand.mTrackedSince = now;
  }

  // 1. The index fingertip, smoothed three ways:
  // - very lightly, for when presses start and end ('depth')
  // - lightly, for where presses land ('touch')
  // - a bit more, for hovering ('hover')
  const Vector3 rawTip = joints[Joint::IndexTip].mPosition;
  const Vector3 hoverTip = hand.mHoverFilter.Update(
    rawTip, dt, s.mHoverMinCutoff, s.mHoverBeta, s.mDerivativeCutoff);
  const Vector3 touchTip = hand.mTouchFilter.Update(
    rawTip, dt, s.mTouchMinCutoff, s.mTouchBeta, s.mDerivativeCutoff);
  const Vector3 depthTip = hand.mDepthFilter.Update(
    rawTip, dt, s.mDepthMinCutoff, s.mDepthBeta, s.mDerivativeCutoff);

  const auto depth = ProjectOntoPanel(depthTip, keyboard, viewerPosition);
  if (!(std::isfinite(depth.mDistance) && std::isfinite(depth.mEdgeDistance)
        && std::isfinite(depth.mU) && std::isfinite(depth.mV))) {
    // The keyboard's position isn't usable
    ResetHand(hand, now, false);
    diag = {};
    return;
  }
  const Vector3& normal = depth.mFrontNormal;

  // 2. Tracking noise, as in `Tracker`. The second difference cancels out
  // smooth movement, leaving mostly jitter. Rises quickly, falls slowly; each
  // frame can only raise it a little, so one sudden stop or start isn't
  // mistaken for jitter.
  if (dt > 0 || hand.mRecentDepthTipCount == 0) {
    if (hand.mRecentDepthTipCount >= 2) {
      const float secondDifference
        = (depthTip - (hand.mRecentDepthTips[1] * 2.0f)
           + hand.mRecentDepthTips[0])
            .Length();
      const float sample = std::min(
        secondDifference / 2.65f, (hand.mTrackingNoise * 2.0f) + 0.0002f);
      const float timeConstant = (sample > hand.mTrackingNoise)
        ? (s.mNoiseTimeConstant / 5)
        : s.mNoiseTimeConstant;
      hand.mTrackingNoise += (sample - hand.mTrackingNoise)
        * std::clamp(dt / timeConstant, 0.0f, 1.0f);
    }
    hand.mRecentDepthTips[0] = hand.mRecentDepthTips[1];
    hand.mRecentDepthTips[1] = depthTip;
    hand.mRecentDepthTipCount = std::min(hand.mRecentDepthTipCount + 1, 2);
  }
  // How far the fingertip must pull back to end a press, and push in to start
  // one from inside the touch zone
  const float stroke = std::clamp(
    s.mRetractNoiseMultiplier * hand.mTrackingNoise,
    s.mMinRetractDistance,
    std::max(s.mMaxRetractDistance, s.mMinRetractDistance));

  // 3. Recent fingertips: how fast it's moving toward the keyboard, and how
  // far it has come
  hand.mHistory[hand.mHistoryNext] = {now, depthTip, depth.mDistance};
  hand.mHistoryNext = (hand.mHistoryNext + 1) % HistorySize;
  hand.mHistoryCount = std::min(hand.mHistoryCount + 1, HistorySize);
  // Age 0 is this frame's
  const auto sampleAt = [&](std::size_t age) -> const Sample& {
    return hand.mHistory
      [(hand.mHistoryNext + (2 * HistorySize) - 1 - age) % HistorySize];
  };
  std::optional<Vector3> velocity;
  float velocitySpan = 0;
  if (hand.mHistoryCount >= 2) {
    std::size_t age = 1;
    while (age + 1 < hand.mHistoryCount
           && now - sampleAt(age).mTime < s.mVelocityWindow) {
      ++age;
    }
    const auto& from = sampleAt(age);
    const auto span = static_cast<float>(now - from.mTime);
    if (span > 1e-4f) {
      velocity = (depthTip - from.mTip) / span;
      velocitySpan = span;
    }
  }
  const float approachSpeed = velocity ? -velocity->Dot(normal) : 0.0f;
  // The furthest out the fingertip has been since `since`, within the stroke
  // window
  const auto highestSince = [&](double since) {
    const double from = std::max(since, now - s.mStrokeWindow);
    float highest = depth.mDistance;
    for (std::size_t age = 1; age < hand.mHistoryCount; ++age) {
      const auto& sample = sampleAt(age);
      if (sample.mTime < from) {
        break;
      }
      highest = std::max(highest, sample.mDistance);
    }
    return highest;
  };

  // 4. Is the hand near the keyboard, for the pointing tracker: any part of
  // the index fingertip within the hover zone, with hysteresis
  {
    const float radius = std::max(joints[Joint::IndexTip].mRadius, 0.0f);
    const float extra = hand.mIsNear ? s.mHoverHysteresis : 0.0f;
    hand.mIsNear = (depth.mDistance - radius <= s.mHoverDistance + extra)
      && (depth.mDistance + radius >= -(s.mMaxPushThrough + extra))
      && (depth.mEdgeDistance - radius <= s.mEdgeMargin + extra);
  }

  // 5. Ignored hands: a fist, or resting on the keyboard
  const float indexCurl = GetFingerCurls(joints).mIndex;
  hand.mIsFist = indexCurl
    > (hand.mIsFist ? s.mFistIndexReleaseCurl : s.mFistIndexCurl);
  uint8_t restingTips = 0;
  for (const auto tip: FingerTips) {
    const auto at
      = ProjectOntoPanel(joints[tip].mPosition, keyboard, viewerPosition);
    if (
      at.mEdgeDistance <= s.mRestingEdgeMargin
      && at.mDistance <= s.mRestingDistance
      && at.mDistance >= -s.mMaxPushThrough) {
      ++restingTips;
    }
  }
  const bool isResting = restingTips >= s.mRestingFingertips;
  const bool ignored = hand.mIsFist || isResting;

  // 6. State machine
  const float hysteresis
    = (hand.mPhase != TypingPhase::Idle) ? s.mHoverHysteresis : 0.0f;
  const bool inZone = (!ignored)
    && depth.mDistance <= s.mHoverDistance + hysteresis
    && depth.mDistance >= -s.mMaxPushThrough
    && depth.mEdgeDistance <= s.mEdgeMargin + hysteresis;
  const float rearmDistance = s.mTouchDistance + s.mRearmDistance;
  const bool settled = (now - *hand.mTrackedSince) >= s.mReacquireDelay;
  const bool cooledDown
    = (!hand.mReleaseTime) || (now - *hand.mReleaseTime) >= s.mPressCooldown;

  // A press can start if the fingertip is over the keyboard, moving toward it
  // quickly enough, and has come far enough toward it recently (so tracking
  // jitter can't start one)
  const auto canPress = [&](double strokeSince) {
    return settled && cooledDown && depth.mEdgeDistance <= s.mEdgeMargin
      && approachSpeed >= s.mMinApproachSpeed
      && highestSince(strokeSince) - depth.mDistance >= stroke;
  };
  const auto enterInZone
    = [&](double duration, std::optional<double> strokeSince) {
        hand.mPhase = TypingPhase::InZone;
        hand.mInZoneUntil = now + duration;
        hand.mInZoneSince = strokeSince;
      };
  // Where `at` is still in front of the keyboard, the press lands where the
  // fingertip is heading: along the way it's moving, rather than straight
  // onto the keyboard. Coming in at an angle (e.g. reaching down to a
  // keyboard) would otherwise land above where the finger meets it.
  const auto press = [&](const Vector3& at) {
    Vector3 slope {};
    if (velocity && approachSpeed > 0) {
      const Vector3 rawSlope
        = (*velocity + (normal * approachSpeed)) / approachSpeed;
      // Shrink the estimate toward straight in by how much of it could be
      // tracking noise, so noise doesn't scatter presses
      const float noise
        = (hand.mTrackingNoise * std::numbers::sqrt2_v<float> / velocitySpan)
        / approachSpeed;
      const float slopeSquared = rawSlope.LengthSquared();
      slope = rawSlope
        * (slopeSquared / (slopeSquared + (2 * noise * noise) + 1e-12f));
      const float length = slope.Length();
      if (length > s.mMaxApproachSlope) {
        slope *= std::max(s.mMaxApproachSlope, 0.0f) / length;
      }
    }
    const auto atProjection = ProjectOntoPanel(at, keyboard, viewerPosition);
    const auto landing = ProjectOntoPanel(
      at + (slope * std::max(atProjection.mDistance, 0.0f)),
      keyboard,
      viewerPosition);
    hand.mPhase = TypingPhase::Pressed;
    hand.mPressUV = {landing.mU, landing.mV};
    hand.mPressStartTime = now;
    hand.mDeepest = depth.mDistance;
    ++hand.mPressCount;
  };
  bool isHold = false;

  if (!inZone) {
    if (hand.mPhase == TypingPhase::Pressed) {
      hand.mReleaseTime = now;
    }
    hand.mPhase = TypingPhase::Idle;
  } else {
    switch (hand.mPhase) {
      case TypingPhase::Idle:
        // Presses must approach from the front: arriving in the touch zone
        // any other way (from the side, from behind, or when tracking
        // starts) is blocked
        hand.mPhase = (depth.mDistance > s.mTouchDistance)
          ? TypingPhase::Hovering
          : TypingPhase::Blocked;
        break;
      case TypingPhase::Blocked:
        if (depth.mDistance > rearmDistance) {
          hand.mPhase = TypingPhase::Hovering;
        }
        break;
      case TypingPhase::Hovering:
        if (depth.mDistance > s.mTouchDistance) {
          break;
        }
        if (canPress(Forever)) {
          // The press lands where the fingertip crossed the touch plane,
          // between the last frame and this one
          Vector3 crossing = touchTip;
          if (
            hand.mPreviousTouchTip
            && hand.mPreviousDistance > s.mTouchDistance) {
            const float span = hand.mPreviousDistance - depth.mDistance;
            if (span > 1e-5f) {
              const float t = std::clamp(
                (hand.mPreviousDistance - s.mTouchDistance) / span,
                0.0f,
                1.0f);
              crossing = Vector3::Lerp(*hand.mPreviousTouchTip, touchTip, t);
            }
          }
          press(crossing);
        } else {
          // Too slow (or too soon): it can still press if it speeds up
          enterInZone(s.mSlowEntryTime, std::nullopt);
        }
        break;
      case TypingPhase::InZone:
        if (depth.mDistance > rearmDistance) {
          hand.mPhase = TypingPhase::Hovering;
        } else if (
          depth.mDistance <= s.mTouchDistance
          && canPress(hand.mInZoneSince.value_or(Forever))) {
          // Pushing in again without coming out first, e.g. a double letter
          press(touchTip);
        } else if (now >= hand.mInZoneUntil) {
          hand.mPhase = TypingPhase::Blocked;
        }
        break;
      case TypingPhase::Pressed: {
        // Quick presses end with a small pull-back. Holds need a deliberate
        // one, so they aren't ended by the hand drifting or wobbling.
        isHold = (now - hand.mPressStartTime) > s.mLongPressTime;
        if (depth.mDistance < hand.mDeepest) {
          hand.mDeepest = depth.mDistance;
        } else if (isHold && s.mHoldRelaxTime > 0) {
          hand.mDeepest += (depth.mDistance - hand.mDeepest)
            * std::clamp(dt / s.mHoldRelaxTime, 0.0f, 1.0f);
        }
        const float retract
          = isHold ? std::max(stroke, s.mHoldRetractDistance) : stroke;
        if (
          depth.mDistance > hand.mDeepest + retract
          || depth.mDistance > rearmDistance) {
          hand.mReleaseTime = now;
          isHold = false;
          if (depth.mDistance > rearmDistance) {
            hand.mPhase = TypingPhase::Hovering;
          } else {
            // The next stroke must start after this release, not count the
            // way in to this press
            enterInZone(s.mRepressTime, now);
          }
        }
        break;
      }
    }
  }

  hand.mPreviousTouchTip = touchTip;
  hand.mPreviousDistance = depth.mDistance;

  // 7. What the caller sees
  const float offsetV = (keyboard.mSize.y > 0)
    ? (s.mTouchOffsetUp / keyboard.mSize.y)
    : 0.0f;
  const auto finger = [&](TypingFingerState state, const Vector2& uv) {
    return TypingFinger {
      .mState = state,
      .mU = std::clamp(uv.x, 0.0f, 1.0f),
      .mV = std::clamp(uv.y - offsetV, 0.0f, 1.0f),
      .mDistance = depth.mDistance,
      .mPressCount = hand.mPressCount,
    };
  };
  switch (hand.mPhase) {
    case TypingPhase::Idle:
      hand.mFinger = {.mPressCount = hand.mPressCount};
      break;
    case TypingPhase::Pressed:
      hand.mFinger = finger(TypingFingerState::Pressed, hand.mPressUV);
      break;
    default: {
      const auto hover = ProjectOntoPanel(hoverTip, keyboard, viewerPosition);
      hand.mFinger
        = finger(TypingFingerState::Hovering, Vector2 {hover.mU, hover.mV});
      break;
    }
  }
  if (!(std::isfinite(hand.mFinger.mU) && std::isfinite(hand.mFinger.mV))) {
    // Never report a position that isn't a number
    ResetHand(hand, now, false);
    diag = {};
    return;
  }

  diag = {
    .mIsTracked = true,
    .mPhase = hand.mPhase,
    .mIndexCurl = indexCurl,
    .mIsFist = hand.mIsFist,
    .mRestingFingertips = restingTips,
    .mIsResting = isResting,
    .mTrackingNoise = hand.mTrackingNoise,
    .mRetractDistance = stroke,
    .mIsHold = isHold,
    .mApproachSpeed = approachSpeed,
    .mDistance = depth.mDistance,
    .mEdgeDistance = depth.mEdgeDistance,
  };
}

}// namespace OpenDesktopVR::HandInteraction
