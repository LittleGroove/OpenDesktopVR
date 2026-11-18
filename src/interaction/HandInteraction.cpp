// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).

#include <OpenDesktopVR/HandInteraction.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace OpenDesktopVR::HandInteraction {

namespace {

// Angle between bone a->b and bone b->c; 0 if they're in a straight line
float BendRadians(const Vector3& a, const Vector3& b, const Vector3& c) {
  const Vector3 u = b - a;
  const Vector3 v = c - b;
  const float lu = u.Length();
  const float lv = v.Length();
  if (lu < 1e-5f || lv < 1e-5f) {
    return 0;
  }
  const float cosAngle = std::clamp(u.Dot(v) / (lu * lv), -1.0f, 1.0f);
  return std::acos(cosAngle);
}

// Sum of the three finger joint bends, scaled so that 180 degrees (a closed
// fist is usually more than this) is fully curled
float Curl(const HandJoints& hand, Joint metacarpal) {
  const auto base = static_cast<std::size_t>(metacarpal);
  const auto at = [&](std::size_t offset) -> const Vector3& {
    return hand.mJoints[base + offset].mPosition;
  };
  const float total = BendRadians(at(0), at(1), at(2))
    + BendRadians(at(1), at(2), at(3)) + BendRadians(at(2), at(3), at(4));
  return std::clamp(total / std::numbers::pi_v<float>, 0.0f, 1.0f);
}

float SmoothingFactor(float cutoffHz, float dt) {
  const float tau = 1.0f / (2.0f * std::numbers::pi_v<float> * cutoffHz);
  return 1.0f / (1.0f + (tau / dt));
}

// NaN or infinity from hand tracking
// would stick in the smoothing filters, and end up in the cursor position
bool IsUsable(const HandJoints& hand) {
  if (!hand.mIsTracked) {
    return false;
  }
  return std::ranges::all_of(hand.mJoints, [](const JointLocation& joint) {
    return std::isfinite(joint.mPosition.x) && std::isfinite(joint.mPosition.y)
      && std::isfinite(joint.mPosition.z) && std::isfinite(joint.mRadius);
  });
}

}// namespace

FingerCurls GetFingerCurls(const HandJoints& hand) {
  return {
    .mIndex = Curl(hand, Joint::IndexMetacarpal),
    .mMiddle = Curl(hand, Joint::MiddleMetacarpal),
    .mRing = Curl(hand, Joint::RingMetacarpal),
    .mLittle = Curl(hand, Joint::LittleMetacarpal),
  };
}

// 'touch limiting', for drawing only.
// The hand is drawn on top of the panels, so a fingertip pressed slightly
// through a panel (as every tap and key press is) would look in front of the
// panel while your eyes see it behind: conflicting depth, right where you're
// looking. The drawn hand stops at the surface instead; taps and key presses
// still use where the finger really is.
HandJoints LimitToSurfaces(
  const HandJoints& hand,
  std::span<const Panel> panels,
  const std::optional<Panel>& keyboard,
  const Vector3& viewerPosition) noexcept {
  // Further through than this, the hand is reaching behind the panel, and is
  // drawn where it is
  constexpr float maxPushThrough = 0.08f;
  constexpr float edgeMargin = 0.01f;
  float shift = 0;
  Vector3 direction {};
  const auto consider = [&](const Panel& panel) {
    // The fingertip that goes furthest into this panel
    float deepest = 0;
    Vector3 normal {};
    for (const auto tip: {
           Joint::ThumbTip,
           Joint::IndexTip,
           Joint::MiddleTip,
           Joint::RingTip,
           Joint::LittleTip,
         }) {
      const auto& joint = hand[tip];
      const auto projection
        = ProjectOntoPanel(joint.mPosition, panel, viewerPosition);
      if (projection.mEdgeDistance > edgeMargin) {
        continue;
      }
      // How far the drawn fingertip goes into the surface
      const float into = joint.mRadius - projection.mDistance;
      if (!std::isfinite(into) || into <= deepest) {
        continue;
      }
      deepest = into;
      normal = projection.mFrontNormal;
    }
    // Any fingertip far through: the hand is reaching behind this panel, not
    // pressing it
    if (deepest > maxPushThrough || deepest <= shift) {
      return;
    }
    shift = deepest;
    direction = normal;
  };
  for (const auto& panel: panels) {
    consider(panel);
  }
  if (keyboard) {
    consider(*keyboard);
  }
  if (shift <= 0 || !std::isfinite(shift)) {
    return hand;
  }
  auto ret = hand;
  for (auto& joint: ret.mJoints) {
    joint.mPosition += direction * shift;
  }
  return ret;
}

float DistanceToPanel(const Vector3& point, const Panel& panel) {
  const auto projection = ProjectOntoPanel(point, panel, point);
  return std::sqrt(
    (projection.mDistance * projection.mDistance)
    + (projection.mEdgeDistance * projection.mEdgeDistance));
}

PanelProjection ProjectOntoPanel(
  const Vector3& point,
  const Panel& panel,
  const Vector3& viewerPosition) {
  Quaternion inverse;
  panel.mOrientation.Inverse(inverse);
  const Vector3 local = Vector3::Transform(point - panel.mPosition, inverse);

  const Vector3 normal = Vector3::Transform(Vector3::UnitZ, panel.mOrientation);
  // Quads are visible from both sides; 'front' is whichever side the viewer is
  // on
  const float side = (viewerPosition - panel.mPosition).Dot(normal) >= 0
    ? 1.0f
    : -1.0f;

  const float halfWidth = panel.mSize.x / 2;
  const float halfHeight = panel.mSize.y / 2;
  const float outsideX = std::max(std::abs(local.x) - halfWidth, 0.0f);
  const float outsideY = std::max(std::abs(local.y) - halfHeight, 0.0f);

  return {
    .mU = (panel.mSize.x > 0) ? ((local.x / panel.mSize.x) + 0.5f) : 0.5f,
    .mV = (panel.mSize.y > 0) ? (0.5f - (local.y / panel.mSize.y)) : 0.5f,
    .mDistance = local.z * side,
    .mEdgeDistance = std::sqrt((outsideX * outsideX) + (outsideY * outsideY)),
    .mFrontNormal = normal * side,
  };
}

Vector3 OneEuroFilter::Update(
  const Vector3& value,
  float dt,
  float minCutoff,
  float beta,
  float derivativeCutoff) {
  if (!mHaveValue) {
    mHaveValue = true;
    mValue = value;
    mDerivative = {};
    return mValue;
  }
  if (dt <= 0) {
    return mValue;
  }

  const Vector3 rawDerivative = (value - mValue) / dt;
  mDerivative += (rawDerivative - mDerivative)
    * SmoothingFactor(derivativeCutoff, dt);

  const float cutoff = minCutoff + (beta * mDerivative.Length());
  mValue += (value - mValue) * SmoothingFactor(cutoff, dt);
  return mValue;
}

void OneEuroFilter::Reset() noexcept {
  mHaveValue = false;
  mValue = {};
  mDerivative = {};
}

Tracker::Tracker(const Settings& settings) : mSettings(settings) {
}

FrameResult Tracker::Update(
  double nowSeconds,
  const std::array<HandJoints, HandCount>& hands,
  std::span<const Panel> panels,
  const Vector3& viewerPosition) {
  FrameResult result;

  float dt = mLastUpdate ? static_cast<float>(nowSeconds - *mLastUpdate) : 0;
  // If frames stop (e.g. the game is paused), don't jump
  dt = std::clamp(dt, 0.0f, 0.1f);
  mLastUpdate = nowSeconds;

  // Unusable data is treated as not tracked
  const HandJoints untracked {};
  for (std::size_t i = 0; i < HandCount; ++i) {
    UpdateHand(
      mHands[i],
      result.mDiagnostics[i],
      nowSeconds,
      dt,
      IsUsable(hands[i]) ? hands[i] : untracked,
      panels,
      viewerPosition);
  }

  // Only one hand can press at a time. A
  // hand that starts touching while the other one is pressing is ignored until
  // it pulls back, so that when the first hand lets go, the button isn't
  // handed over (a drag from one finger to the other, or a click long after
  // the touch)
  if (mTouchOwner && mHands[*mTouchOwner].mPhase != Phase::Touching) {
    mTouchOwner.reset();
  }
  for (std::size_t i = 0; i < HandCount; ++i) {
    auto& hand = mHands[i];
    if (hand.mPhase != Phase::Touching) {
      continue;
    }
    if (!mTouchOwner) {
      mTouchOwner = i;
      continue;
    }
    if (*mTouchOwner == i) {
      continue;
    }
    hand.mPhase = Phase::Blocked;
    hand.mIsDragging = false;
    hand.mIsHold = false;
    hand.mIsScrolling = false;
    hand.mPendingWheel = 0;
    auto& diag = result.mDiagnostics[i];
    diag.mPhase = hand.mPhase;
    diag.mIsDragging = false;
    diag.mIsHold = false;
  }

  // Only one cursor: a touching hand wins, otherwise the closest hovering one
  const auto showsCursor = [](Phase phase) {
    return phase == Phase::Hovering || phase == Phase::Releasing;
  };
  std::optional<std::size_t> active;
  for (std::size_t i = 0; i < HandCount; ++i) {
    if (mHands[i].mPhase == Phase::Touching) {
      active = i;
      break;
    }
  }
  if (!active) {
    for (std::size_t i = 0; i < HandCount; ++i) {
      if (!showsCursor(mHands[i].mPhase)) {
        continue;
      }
      if (!active || mHands[i].mDistance < mHands[*active].mDistance) {
        active = i;
      }
    }
  }

  std::optional<CursorUpdate> desired;
  if (active) {
    auto& hand = mHands[*active];
    const bool touching = hand.mPhase == Phase::Touching;
    // Scrolling never presses a button, and keeps the pointer where the
    // fingers first touched, like a trackpad
    const bool scrolling = touching && hand.mTouchGesture == Gesture::Scroll;
    const auto uv = (touching && (scrolling || !hand.mIsDragging))
      ? hand.mTouchUV
      : hand.mUV;
    // The user's fine-tuning: +V is down the panel
    float offsetV = 0;
    if (mSettings.mTouchOffsetUp != 0) {
      const auto panel = std::ranges::find(panels, hand.mPanelID, &Panel::mID);
      if (panel != panels.end() && panel->mSize.y > 0) {
        offsetV = mSettings.mTouchOffsetUp / panel->mSize.y;
      }
    }
    desired = CursorUpdate {
      .mPanelID = hand.mPanelID,
      .mU = std::clamp(uv.x, 0.0f, 1.0f),
      .mV = std::clamp(uv.y - offsetV, 0.0f, 1.0f),
      .mState = (touching && !scrolling) ? TouchState::Touching
                                         : TouchState::Hovering,
      .mWheel = std::exchange(hand.mPendingWheel, 0),
    };
    // Never send a position that isn't a
    // number (the app can't read it, which could leave a click held down);
    // without a cursor, any click is released where it was
    if (!(std::isfinite(desired->mU) && std::isfinite(desired->mV))) {
      desired.reset();
      active.reset();
    } else {
      result.mActiveHand = static_cast<Hand>(*active);
    }
  }
  for (auto& hand: mHands) {
    hand.mPendingWheel = 0;
  }
  EmitCursor(result, desired, active);

  for (std::size_t i = 0; i < HandCount; ++i) {
    result.mHandVisibility[i] = mHands[i].mVisibility;
  }
  return result;
}

FrameResult Tracker::Reset() {
  FrameResult result;
  for (auto& hand: mHands) {
    hand = {};
  }
  mLastUpdate.reset();
  mTouchOwner.reset();
  EmitCursor(result, std::nullopt, std::nullopt);
  return result;
}

void Tracker::EmitCursor(
  FrameResult& result,
  const std::optional<CursorUpdate>& desired,
  std::optional<std::size_t> desiredHand) {
  // A click ends exactly where it started (or where a drag ended): some apps
  // ignore clicks if the button is released somewhere else
  //
  // ... including when the other hand takes
  // over, so a press is never handed from one hand to the other
  if (
    mLastSent && mLastSent->mState == TouchState::Touching
    && !(
      desired && desired->mState == TouchState::Touching
      && desired->mPanelID == mLastSent->mPanelID
      && desiredHand == mLastSentHand)) {
    mLastSent->mState = TouchState::Hovering;
    result.mCursorUpdates.Push(*mLastSent);
  }

  const auto leaveLast = [&]() {
    result.mCursorUpdates.Push({
      .mPanelID = mLastSent->mPanelID,
      .mU = mLastSent->mU,
      .mV = mLastSent->mV,
      .mState = TouchState::NotNear,
    });
    mLastSent.reset();
    mLastSentHand.reset();
  };

  if (!desired) {
    if (mLastSent) {
      leaveLast();
    }
    return;
  }

  if (mLastSent && mLastSent->mPanelID != desired->mPanelID) {
    leaveLast();
  }

  if (
    mLastSent && mLastSent->mState == desired->mState
    && std::abs(mLastSent->mU - desired->mU) < 0.0005f
    && std::abs(mLastSent->mV - desired->mV) < 0.0005f
    && desired->mWheel == 0) {
    return;
  }

  result.mCursorUpdates.Push(*desired);
  mLastSent = desired;
  mLastSentHand = desiredHand;
  // Wheel movement is sent once
  mLastSent->mWheel = 0;
}

void Tracker::UpdateHand(
  HandState& hand,
  HandDiagnostics& diag,
  double now,
  float dt,
  const HandJoints& joints,
  std::span<const Panel> panels,
  const Vector3& viewerPosition) {
  const auto& s = mSettings;

  const auto fade = [&](bool visible) {
    const float step = (s.mFadeDuration > 0) ? (dt / s.mFadeDuration) : 1.0f;
    hand.mVisibility
      = std::clamp(hand.mVisibility + (visible ? step : -step), 0.0f, 1.0f);
  };
  const auto enterIdle = [&]() {
    hand.mPhase = Phase::Idle;
    hand.mIsDragging = false;
    hand.mIsHold = false;
  };

  diag.mIsTracked = joints.mIsTracked;
  if (!joints.mIsTracked) {
    // Releases any touch: the cursor logic will send 'not near'
    hand.mGesture = Gesture::None;
    hand.mGestureSince.reset();
    hand.mArmedGesture = Gesture::None;
    hand.mNotArmedGestureSince.reset();
    hand.mIsArmed = false;
    hand.mContactPointFor = Gesture::None;
    hand.mIsScrolling = false;
    hand.mPendingWheel = 0;
    hand.mCursorFilter.Reset();
    hand.mTouchFilter.Reset();
    hand.mDepthFilter.Reset();
    hand.mRecentDepthTipCount = 0;
    hand.mApproachCount = 0;
    hand.mPreviousPanelID.reset();
    // The noise estimate is otherwise
    // kept across tracking loss on purpose (it falls slowly), but must not
    // stay stuck at NaN or infinity
    if (!std::isfinite(hand.mTrackingNoise)) {
      hand.mTrackingNoise = HandState {}.mTrackingNoise;
    }
    enterIdle();
    fade(false);
    diag.mPhase = hand.mPhase;
    return;
  }

  // 1. Hand pose, with hysteresis
  const auto curls = GetFingerCurls(joints);
  diag.mCurls = curls;
  // 'holding' uses the looser thresholds, to keep a pose that's already held
  const auto isPoint = [&](bool holding) {
    return holding
      ? (curls.mIndex <= s.mPointIndexReleaseCurl
         && curls.mMiddle >= s.mPointOthersReleaseCurl
         && curls.mRing >= s.mPointOthersReleaseCurl)
      : (curls.mIndex <= s.mPointIndexMaxCurl
         && curls.mMiddle >= s.mPointOthersMinCurl
         && curls.mRing >= s.mPointOthersMinCurl);
  };
  const auto isScroll = [&](bool holding) {
    return holding
      ? (curls.mIndex <= s.mPointIndexReleaseCurl
         && curls.mMiddle <= s.mPointIndexReleaseCurl
         && curls.mRing >= s.mPointOthersReleaseCurl)
      : (curls.mIndex <= s.mPointIndexMaxCurl
         && curls.mMiddle <= s.mPointIndexMaxCurl
         && curls.mRing >= s.mPointOthersMinCurl);
  };
  Gesture gesture = Gesture::None;
  if (hand.mGesture == Gesture::Point && isPoint(true)) {
    gesture = Gesture::Point;
  } else if (hand.mGesture == Gesture::Scroll && isScroll(true)) {
    gesture = Gesture::Scroll;
  } else if (isPoint(false)) {
    gesture = Gesture::Point;
  } else if (isScroll(false)) {
    gesture = Gesture::Scroll;
  }
  if (gesture != hand.mGesture) {
    hand.mGesture = gesture;
    hand.mGestureSince = now;
  }

  // 2. A pose enables interaction ('arms') once it has been held briefly. A
  // touch that has started always finishes, even if the pose changes.
  if (hand.mGesture != Gesture::None && hand.mGesture == hand.mArmedGesture) {
    hand.mNotArmedGestureSince.reset();
  } else if (
    hand.mGesture != Gesture::None && hand.mPhase != Phase::Touching
    && now - *hand.mGestureSince >= s.mArmDelay) {
    // A pose (possibly a different one) has been held long enough
    hand.mArmedGesture = hand.mGesture;
    hand.mNotArmedGestureSince.reset();
  } else if (hand.mArmedGesture != Gesture::None) {
    if (!hand.mNotArmedGestureSince) {
      hand.mNotArmedGestureSince = now;
    }
    if (
      hand.mPhase != Phase::Touching
      && now - *hand.mNotArmedGestureSince >= s.mDisarmDelay) {
      hand.mArmedGesture = Gesture::None;
      hand.mNotArmedGestureSince.reset();
    }
  }
  hand.mIsArmed = (hand.mArmedGesture != Gesture::None);

  // 3. The contact point: the index fingertip for tapping, or between the
  // index and middle fingertips for scrolling. If it changes, it jumps by
  // about 1cm, so don't smooth across that.
  const Gesture contactFor = (hand.mPhase == Phase::Touching)
    ? hand.mTouchGesture
    : hand.mArmedGesture;
  const Gesture contactPoint
    = (contactFor == Gesture::Scroll) ? Gesture::Scroll : Gesture::Point;
  if (contactPoint != hand.mContactPointFor) {
    if (hand.mContactPointFor != Gesture::None) {
      hand.mCursorFilter.Reset();
      hand.mTouchFilter.Reset();
      hand.mDepthFilter.Reset();
      hand.mRecentDepthTipCount = 0;
      hand.mApproachCount = 0;
      hand.mPreviousPanelID.reset();
    }
    hand.mContactPointFor = contactPoint;
  }
  const bool twoFingers = (contactPoint == Gesture::Scroll);

  // Smoothed three ways:
  // - very lightly, for when taps start and end ('depth')
  // - lightly, for where taps land, and drags ('touch')
  // - a bit more, for the hover cursor ('cursor')
  const Vector3 rawTip = twoFingers
    ? (joints[Joint::IndexTip].mPosition + joints[Joint::MiddleTip].mPosition)
      * 0.5f
    : joints[Joint::IndexTip].mPosition;
  const Vector3 cursorTip = hand.mCursorFilter.Update(
    rawTip, dt, s.mCursorMinCutoff, s.mCursorBeta, s.mDerivativeCutoff);
  const Vector3 touchTip = hand.mTouchFilter.Update(
    rawTip, dt, s.mTouchMinCutoff, s.mTouchBeta, s.mDerivativeCutoff);
  const Vector3 depthTip = hand.mDepthFilter.Update(
    rawTip, dt, s.mDepthMinCutoff, s.mDepthBeta, s.mDerivativeCutoff);
  const Vector3 fingerBase = twoFingers
    ? (joints[Joint::IndexProximal].mPosition
       + joints[Joint::MiddleProximal].mPosition)
      * 0.5f
    : joints[Joint::IndexProximal].mPosition;
  Vector3 fingerDirection = rawTip - fingerBase;
  fingerDirection.Normalize();

  // Tracking noise. The second difference cancels out smooth movement,
  // leaving mostly jitter; for jitter, its length averages about 2.65 times
  // the noise on each axis. Rises quickly, falls slowly. Each frame can only
  // raise it a little, so one sudden stop or start isn't mistaken for jitter;
  // jitter keeps raising it.
  if (hand.mRecentDepthTipCount >= 2 && dt > 0) {
    const float secondDifference = (depthTip
                                    - (hand.mRecentDepthTips[1] * 2.0f)
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

  // The last few fingertips, for the direction a touch comes in from
  {
    auto& count = hand.mApproachCount;
    constexpr int capacity
      = static_cast<int>(std::tuple_size_v<decltype(hand.mApproachTips)>);
    if (count == capacity) {
      std::shift_left(hand.mApproachTips.begin(), hand.mApproachTips.end(), 1);
      std::shift_left(hand.mApproachTimes.begin(), hand.mApproachTimes.end(), 1);
      --count;
    }
    hand.mApproachTips[count] = depthTip;
    hand.mApproachTimes[count] = now;
    ++count;
  }
  const float tapRetractDistance = std::clamp(
    s.mRetractNoiseMultiplier * hand.mTrackingNoise,
    s.mFastRetractDistance,
    s.mRetractDistance);

  // 4. Find the panel the fingertip is on
  struct Candidate {
    const Panel* mPanel {nullptr};
    // When taps start and end, and which panel
    PanelProjection mDepth {};
    // Where taps land, and drags
    PanelProjection mTouch {};
    // Hover cursor position
    PanelProjection mCursor {};
  };
  std::optional<Candidate> best;
  float bestScore = std::numeric_limits<float>::infinity();
  float nearestScore = std::numeric_limits<float>::infinity();
  diag.mNearestPanelID.reset();

  for (const auto& panel: panels) {
    const auto depth = ProjectOntoPanel(depthTip, panel, viewerPosition);

    // Diagnostics: nearest panel, ignoring all limits
    const float nearest = std::abs(depth.mDistance) + depth.mEdgeDistance;
    if (nearest < nearestScore) {
      nearestScore = nearest;
      diag.mNearestPanelID = panel.mID;
      diag.mNearest = depth;
    }

    // Be a little sticky to the current panel
    const bool isCurrent
      = (hand.mPhase != Phase::Idle) && (panel.mID == hand.mPanelID);
    const float edgeMargin = s.mEdgeMargin + (isCurrent ? 0.01f : 0.0f);
    const float hoverDistance
      = s.mHoverDistance + (isCurrent ? 0.02f : 0.0f);
    if (
      depth.mEdgeDistance > edgeMargin || depth.mDistance > hoverDistance
      || depth.mDistance < -s.mMaxPushThrough) {
      continue;
    }

    float score = std::max(depth.mDistance, 0.0f) + depth.mEdgeDistance;
    if (isCurrent) {
      score -= 0.05f;
    }
    // `<=`, so on a tie (panels overlapping
    // in the same place) the one drawn later, on top, wins; panels are in draw
    // order
    if (score <= bestScore) {
      bestScore = score;
      best = Candidate {
        .mPanel = &panel,
        .mDepth = depth,
        .mTouch = ProjectOntoPanel(touchTip, panel, viewerPosition),
        .mCursor = ProjectOntoPanel(cursorTip, panel, viewerPosition),
      };
    }
  }

  const float aimCosine
    = best ? fingerDirection.Dot(-best->mDepth.mFrontNormal) : 0.0f;
  const auto trackBest = [&]() {
    hand.mPanelID = best->mPanel->mID;
    hand.mUV = {best->mCursor.mU, best->mCursor.mV};
    hand.mDistance = best->mDepth.mDistance;
  };
  const auto onSamePanel
    = [&]() { return best && best->mPanel->mID == hand.mPanelID; };
  const float rearmDistance = s.mTouchDistance + s.mRearmDistance;

  // 5. State machine
  if (hand.mPhase == Phase::Blocked) {
    // Wait for the fingertip to pull back or leave the panel
    if ((!onSamePanel()) || best->mDepth.mDistance > s.mReleaseDistance) {
      enterIdle();
    }
  } else if (hand.mPhase == Phase::Releasing) {
    if ((!onSamePanel()) || !hand.mIsArmed) {
      enterIdle();
    } else if (best->mDepth.mDistance > rearmDistance) {
      // Ready to tap again
      trackBest();
      hand.mPhase = Phase::Hovering;
    } else {
      trackBest();
    }
  }

  if (hand.mPhase == Phase::Touching) {
    const bool stillOnPanel = onSamePanel();
    const float elapsed = static_cast<float>(now - hand.mTouchStartTime);
    // Sideways movement since the touch landed, tracked along the direction
    // the finger came in
    const auto alongApproach = [&](const PanelProjection& at) {
      return touchTip + (hand.mApproachSlope * at.mDistance);
    };
    float movedAcross = 0;
    if (stillOnPanel) {
      const float depth = best->mDepth.mDistance;
      hand.mDeepest = std::min(hand.mDeepest, depth);
      const auto& normal = best->mTouch.mFrontNormal;
      Vector3 moved = alongApproach(best->mTouch) - hand.mTouchTip;
      moved -= normal * moved.Dot(normal);
      movedAcross = moved.Length();
      if (hand.mPreviousPanelID == best->mPanel->mID && dt > 0) {
        Vector3 step = touchTip - hand.mPreviousTouchTip;
        step -= normal * step.Dot(normal);
        hand.mLateralSpeed += ((step.Length() / dt) - hand.mLateralSpeed)
          * std::clamp(dt / 0.05f, 0.0f, 1.0f);
        hand.mRetractSpeed += (((depth - hand.mPreviousDistance) / dt)
                               - hand.mRetractSpeed)
          * std::clamp(dt / 0.03f, 0.0f, 1.0f);
      }
    }
    // Quick taps end with a small pull-back. Holds (long touches, and ones
    // that move, which may become drags) need a deliberate one, so they aren't
    // ended by the hand drifting.
    if (
      (!hand.mIsHold)
      && (hand.mIsDragging || elapsed > s.mLongPressTime || movedAcross > s.mHoldMoveDistance)) {
      hand.mIsHold = true;
      hand.mHoldDepth = hand.mDeepest;
    }
    bool released = !stillOnPanel;
    bool pullingBack = false;
    if (stillOnPanel && hand.mIsHold) {
      const float depth = best->mDepth.mDistance;
      if (depth < hand.mHoldDepth) {
        hand.mHoldDepth = depth;
      } else {
        // Follow slow drift; a quick pull-back gets ahead of this
        hand.mHoldDepth += (depth - hand.mHoldDepth)
          * std::clamp(dt / s.mHoldRelaxTime, 0.0f, 1.0f);
      }
      const float retract = s.mHoldRetractDistance
        + std::min(s.mHoldRetractPerSpeed * hand.mLateralSpeed, s.mHoldRetractMaxExtra);
      released = depth > s.mHoldReleaseDistance
        || depth > hand.mHoldDepth + retract;
      pullingBack = hand.mRetractSpeed > s.mRetractFreezeSpeed
        && depth > hand.mHoldDepth + 0.002f;
    } else if (stillOnPanel) {
      released = best->mDepth.mDistance > s.mReleaseDistance
        || best->mDepth.mDistance > hand.mDeepest + tapRetractDistance;
    }
    if (released) {
      hand.mIsDragging = false;
      hand.mIsHold = false;
      if (stillOnPanel && hand.mIsArmed) {
        trackBest();
        hand.mPhase = (best->mDepth.mDistance > rearmDistance)
          ? Phase::Hovering
          : Phase::Releasing;
      } else {
        enterIdle();
      }
    } else {
      if (hand.mTouchGesture == Gesture::Scroll) {
        // Natural scrolling: the content follows the fingers. Movement while
        // pulling back to let go doesn't count.
        const float moved = pullingBack
          ? 0.0f
          : (best->mTouch.mV - hand.mScrollV) * best->mPanel->mSize.y;
        hand.mScrollV = best->mTouch.mV;
        hand.mScrollTravel += moved;
        if (
          (!hand.mIsScrolling)
          && std::abs(hand.mScrollTravel) >= s.mScrollStartDistance) {
          hand.mIsScrolling = true;
        }
        if (hand.mIsScrolling) {
          const auto notches = static_cast<int32_t>(
            hand.mScrollTravel / s.mScrollDistancePerNotch);
          if (notches != 0) {
            hand.mScrollTravel -= notches * s.mScrollDistancePerNotch;
            hand.mPendingWheel += notches * 120;
          }
        }
      }
      // Ignore small wobbles, but follow deliberate drags. Where a drag
      // points: along the approach at first, fading to straight onto the
      // panel, so the hand drifting in and out doesn't move it.
      const auto dragUV = [&]() {
        const float approach = std::exp(
          -static_cast<float>(now - hand.mDragStartTime)
          / s.mDragApproachFadeTime);
        const auto at = ProjectOntoPanel(
          touchTip
            + (hand.mApproachSlope * (best->mTouch.mDistance * approach)),
          *best->mPanel,
          viewerPosition);
        return Vector2 {at.mU, at.mV};
      };
      if (!hand.mIsDragging) {
        const float dragStart = (elapsed > s.mLongPressTime)
          ? s.mHoldDragStartDistance
          : s.mDragStartDistance;
        if (movedAcross > dragStart) {
          hand.mIsDragging = true;
          hand.mDragStartTime = now;
          // Start from where the click is, then catch up with the finger
          hand.mDragOffset = hand.mTouchUV - dragUV();
        }
      }
      hand.mPanelID = best->mPanel->mID;
      hand.mDistance = best->mDepth.mDistance;
      if (hand.mIsDragging && !pullingBack) {
        hand.mDragOffset *= std::exp(-dt / s.mDragCatchUpTime);
        hand.mUV = dragUV() + hand.mDragOffset;
      }
    }
  } else if (hand.mPhase == Phase::Hovering) {
    if ((!hand.mIsArmed) || !best) {
      enterIdle();
    } else if (best->mPanel->mID != hand.mPanelID) {
      // Moved onto a different panel
      trackBest();
      if (best->mDepth.mDistance <= s.mTouchDistance) {
        hand.mPhase = Phase::Blocked;
      }
    } else if (best->mDepth.mDistance <= s.mTouchDistance) {
      if (aimCosine >= s.mMinAimCosine) {
        // The click lands where the fingertip crossed the touch distance,
        // between the last frame and this one
        Vector3 crossing = touchTip;
        if (
          hand.mPreviousPanelID == best->mPanel->mID
          && hand.mPreviousDistance > s.mTouchDistance) {
          const float span = hand.mPreviousDistance - best->mDepth.mDistance;
          if (span > 1e-5f) {
            const float t = std::clamp(
              (hand.mPreviousDistance - s.mTouchDistance) / span, 0.0f, 1.0f);
            crossing = Vector3::Lerp(hand.mPreviousTouchTip, touchTip, t);
          }
        }
        // That's still in front of the panel, so the click lands where the
        // fingertip is heading: along the way it's moving, rather than
        // straight onto the panel. Coming in at an angle (e.g. reaching down
        // to a panel) would otherwise land above where the finger meets
        // it.
        const auto& normal = best->mDepth.mFrontNormal;
        hand.mApproachSlope = {};
        if (hand.mApproachCount >= 2) {
          const int newest = hand.mApproachCount - 1;
          const auto span = static_cast<float>(
            hand.mApproachTimes[newest] - hand.mApproachTimes[0]);
          if (span > 1e-4f) {
            const Vector3 velocity
              = (hand.mApproachTips[newest] - hand.mApproachTips[0]) / span;
            const float inward = -velocity.Dot(normal);
            if (inward >= s.mMinApproachSpeed) {
              const Vector3 slope = (velocity + (normal * inward)) / inward;
              // Shrink the estimate toward straight in by how much of it
              // could be tracking noise, so noise doesn't scatter taps
              const float noise
                = (hand.mTrackingNoise * std::numbers::sqrt2_v<float> / span)
                / inward;
              const float slopeSquared = slope.LengthSquared();
              hand.mApproachSlope = slope
                * (slopeSquared
                   / (slopeSquared + (2 * noise * noise) + 1e-12f));
              const float length = hand.mApproachSlope.Length();
              if (length > s.mMaxApproachSlope) {
                hand.mApproachSlope *= s.mMaxApproachSlope / length;
              }
            }
          }
        }
        const auto atCrossing
          = ProjectOntoPanel(crossing, *best->mPanel, viewerPosition);
        const Vector3 landing = crossing
          + (hand.mApproachSlope * std::max(atCrossing.mDistance, 0.0f));
        const auto touch
          = ProjectOntoPanel(landing, *best->mPanel, viewerPosition);
        trackBest();
        hand.mPhase = Phase::Touching;
        hand.mTouchGesture = hand.mArmedGesture;
        hand.mTouchUV = {touch.mU, touch.mV};
        hand.mUV = hand.mTouchUV;
        hand.mTouchTip = landing - (normal * touch.mDistance);
        hand.mDeepest = best->mDepth.mDistance;
        hand.mTouchStartTime = now;
        hand.mIsDragging = false;
        hand.mIsHold = false;
        hand.mLateralSpeed = 0;
        hand.mRetractSpeed = 0;
        hand.mDragStartTime = now;
        hand.mDragOffset = {};
        if (hand.mTouchGesture == Gesture::Scroll) {
          // Scrolling is a drag from the start, so drifting doesn't end it
          hand.mIsDragging = true;
          hand.mIsScrolling = false;
          hand.mScrollV = best->mTouch.mV;
          hand.mScrollTravel = 0;
        }
        {
          // For the log: in the panel's directions
          Quaternion inverse;
          best->mPanel->mOrientation.Inverse(inverse);
          const auto local
            = Vector3::Transform(hand.mApproachSlope, inverse);
          hand.mLastApproachSlope = {local.x, -local.y};
        }
      } else {
        hand.mPhase = Phase::Blocked;
      }
    } else {
      trackBest();
    }
  }

  // Includes hands that became idle above
  if (hand.mPhase == Phase::Idle && hand.mIsArmed && best) {
    trackBest();
    // Taps must approach from the front: reaching the panel any other way
    // (sliding in from the side, or from behind) is blocked
    hand.mPhase = (best->mDepth.mDistance > s.mTouchDistance) ? Phase::Hovering
                                                              : Phase::Blocked;
  }

  if (best) {
    hand.mPreviousPanelID = best->mPanel->mID;
    hand.mPreviousTouchTip = touchTip;
    hand.mPreviousDistance = best->mDepth.mDistance;
  } else {
    hand.mPreviousPanelID.reset();
  }

  fade(hand.mIsArmed || hand.mPhase == Phase::Touching);

  diag.mGesture = hand.mGesture;
  diag.mArmedGesture = hand.mArmedGesture;
  diag.mIsPointing = (hand.mGesture != Gesture::None);
  diag.mIsArmed = hand.mIsArmed;
  diag.mPhase = hand.mPhase;
  diag.mAimCosine = aimCosine;
  diag.mTrackingNoise = hand.mTrackingNoise;
  diag.mTapRetractDistance = tapRetractDistance;
  diag.mApproachSlope = hand.mLastApproachSlope;
  diag.mIsHold = hand.mIsHold;
  diag.mIsDragging = hand.mIsDragging;
}

}// namespace OpenDesktopVR::HandInteraction
