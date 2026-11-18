// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Pure logic for "point with your index finger, then tap a panel to
// click". No OpenXR or graphics dependencies, so it can be tested with
// synthetic hands.
#pragma once

#include <SimpleMath.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace OpenDesktopVR::HandInteraction {

// Vector math from DirectXTK's SimpleMath
using DirectX::SimpleMath::Quaternion;
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;

// Same order and values as `XrHandJointEXT`
enum class Joint : uint8_t {
  Palm = 0,
  Wrist,
  ThumbMetacarpal,
  ThumbProximal,
  ThumbDistal,
  ThumbTip,
  IndexMetacarpal,
  IndexProximal,
  IndexIntermediate,
  IndexDistal,
  IndexTip,
  MiddleMetacarpal,
  MiddleProximal,
  MiddleIntermediate,
  MiddleDistal,
  MiddleTip,
  RingMetacarpal,
  RingProximal,
  RingIntermediate,
  RingDistal,
  RingTip,
  LittleMetacarpal,
  LittleProximal,
  LittleIntermediate,
  LittleDistal,
  LittleTip,
};
inline constexpr std::size_t JointCount = 26;

struct JointLocation {
  Vector3 mPosition {};
  Quaternion mOrientation {};
  float mRadius {0.01f};// meters
};

struct HandJoints {
  bool mIsTracked {false};
  std::array<JointLocation, JointCount> mJoints {};

  [[nodiscard]]
  const JointLocation& operator[](Joint joint) const noexcept {
    return mJoints[static_cast<std::size_t>(joint)];
  }
  [[nodiscard]]
  JointLocation& operator[](Joint joint) noexcept {
    return mJoints[static_cast<std::size_t>(joint)];
  }
};

enum class Hand : uint8_t {
  Left = 0,
  Right = 1,
};
inline constexpr std::size_t HandCount = 2;

// What the renderer needs to draw one hand
struct RenderHand {
  // 0 = hidden, 1 = fully visible; fades in and out
  float mVisibility {0};
  HandJoints mJoints {};
};

// A flat panel in VR; it is centered on `mPosition`, lies in its
// local XY plane, and has its normal along local +Z
struct Panel {
  uint64_t mID {};
  Vector3 mPosition {};
  Quaternion mOrientation {};
  Vector2 mSize {};// meters
};

struct Settings {
  // Distances are in meters, times are in seconds

  // The cursor shows when the fingertip is this close in front of a panel
  float mHoverDistance {0.10f};
  // A tap starts when the fingertip is this close (or behind the panel)...
  float mTouchDistance {0.010f};
  // ... and ends as soon as the fingertip is pulled back from the deepest
  // point it reached. For a quick tap, that's as little as this...
  float mFastRetractDistance {0.002f};
  // ... or this many times the measured tracking noise, if that's more, so
  // that jitter can't end a tap early...
  float mRetractNoiseMultiplier {5.5f};
  // ... and never more than this...
  float mRetractDistance {0.008f};
  // ... or, at the latest, when it is this far in front of the panel. The
  // gaps stop jitter from causing double clicks.
  float mReleaseDistance {0.020f};
  // Touches longer than this are holds, not taps...
  float mLongPressTime {0.35f};
  // ... as are touches that move this far across the panel, as they may be
  // the start of a drag
  float mHoldMoveDistance {0.005f};
  // Holds and drags end when the fingertip is pulled back this far from how
  // deep it's held. The hand drifting while it holds or drags is followed
  // (with this time constant) rather than counted, so only a deliberate
  // pull-back ends them...
  float mHoldRetractDistance {0.010f};
  float mHoldRelaxTime {0.25f};
  // ... plus this much per meter/second of sideways speed, up to the maximum,
  // as fast drags wobble more...
  float mHoldRetractPerSpeed {0.02f};
  float mHoldRetractMaxExtra {0.010f};
  // ... or, at the latest, when the fingertip is this far in front of the
  // panel
  float mHoldReleaseDistance {0.015f};
  // While a hold or drag is being pulled back faster than this (m/s), the
  // pointer stays put, so letting go of a slider doesn't move it
  float mRetractFreezeSpeed {0.06f};
  // After a tap ends, the fingertip must be this much further out than
  // `mTouchDistance` before it can tap again
  float mRearmDistance {0.004f};
  // Fingertips further behind the panel than this are ignored
  float mMaxPushThrough {0.08f};
  // How far outside a panel's edges still counts as on the panel
  float mEdgeMargin {0.010f};
  // The finger must point into the panel: cosine of the largest allowed angle
  // from straight-in (0.4 is about 66 degrees)
  float mMinAimCosine {0.40f};
  // While touching, the click stays where the finger first touched until the
  // fingertip moves this far across the panel; after that, it drags. Holds
  // (see mLongPressTime) start dragging sooner.
  float mDragStartDistance {0.012f};
  float mHoldDragStartDistance {0.008f};
  // When a drag starts, the pointer catches up with the fingertip with this
  // time constant, instead of jumping
  float mDragCatchUpTime {0.06f};
  // Taps are tracked along the direction the finger came in (see
  // mMaxApproachSlope); once dragging, that fades out with this time
  // constant, so the hand drifting in and out doesn't move the pointer
  float mDragApproachFadeTime {0.25f};
  // A tap starts before the fingertip reaches the panel (mTouchDistance), so
  // it lands where the fingertip is heading: along its approach, as long as
  // it is approaching at least this fast (m/s)...
  float mMinApproachSpeed {0.05f};
  // ... and up to this angle from straight in, as a slope (1 = 45 degrees)
  float mMaxApproachSlope {1.0f};
  // Moves taps and the cursor up the panel by this much (negative = down),
  // to correct for a personal offset
  float mTouchOffsetUp {0.0f};
  // The pointing pose must be held this long before taps are enabled
  float mArmDelay {0.08f};
  // ... and taps stay enabled this long after it is lost
  float mDisarmDelay {0.15f};
  // How long the hand takes to fade in or out
  float mFadeDuration {0.12f};

  // Finger curl: 0 = straight, 1 = fully curled.
  // Pointing (to tap) = index straight, middle and ring curled.
  // Scrolling = index and middle straight, ring curled.
  // The little finger and thumb are ignored. The 'release' values add
  // hysteresis.
  float mPointIndexMaxCurl {0.35f};
  float mPointIndexReleaseCurl {0.50f};
  float mPointOthersMinCurl {0.55f};
  float mPointOthersReleaseCurl {0.40f};

  // Scrolling starts once the fingertips have moved this far while
  // touching...
  float mScrollStartDistance {0.004f};
  // ... then scrolls one mouse wheel 'notch' per this much movement
  float mScrollDistancePerNotch {0.012f};

  // One Euro filter parameters for smoothing the fingertip. Lower cutoffs are
  // steadier but lag behind the finger; higher 'beta' reduces lag while
  // moving.
  //
  // The 'cursor' filter is only for the hover position. Where taps land, and
  // drags, use the lightly-smoothed 'touch' filter. When taps start and end
  // uses the very lightly smoothed 'depth' filter, so it reacts within a
  // frame.
  float mCursorMinCutoff {4.0f};// Hz
  float mCursorBeta {20.0f};
  float mTouchMinCutoff {8.0f};// Hz
  float mTouchBeta {20.0f};
  float mDepthMinCutoff {15.0f};// Hz
  float mDepthBeta {30.0f};
  // How quickly the filters notice a change in speed
  float mDerivativeCutoff {2.5f};// Hz
  // How quickly the tracking noise estimate follows changes
  float mNoiseTimeConstant {0.5f};// seconds
};

struct FingerCurls {
  float mIndex {};
  float mMiddle {};
  float mRing {};
  float mLittle {};
};

[[nodiscard]]
FingerCurls GetFingerCurls(const HandJoints&);

struct PanelProjection {
  // 0 = left edge, 1 = right edge
  float mU {};
  // 0 = top edge, 1 = bottom edge
  float mV {};
  // Meters in front of the panel (the side the viewer is on); negative values
  // are behind it
  float mDistance {};
  // Meters outside the panel's edges, or 0 if within them
  float mEdgeDistance {};
  // Unit vector, pointing out of the panel toward the viewer
  Vector3 mFrontNormal {};
};

[[nodiscard]]
PanelProjection ProjectOntoPanel(
  const Vector3& point,
  const Panel&,
  const Vector3& viewerPosition);

// From `point` to the nearest point on
// the panel (its surface, or an edge), in meters. Used to draw panels back to
// front, so a nearer panel is never covered by one that's further away.
float DistanceToPanel(const Vector3& point, const Panel&);

// Where to draw a hand, so it doesn't
// look like it's poking through a panel: a fingertip slightly behind a panel's
// surface (up to 8cm) moves the whole drawn hand back to the surface ('touch
// limiting'). For drawing only; touches use where the hand really is.
[[nodiscard]]
HandJoints LimitToSurfaces(
  const HandJoints&,
  std::span<const Panel> panels,
  const std::optional<Panel>& keyboard,
  const Vector3& viewerPosition) noexcept;

enum class TouchState : uint8_t {
  NotNear = 0,
  Hovering = 1,
  Touching = 2,
};

struct CursorUpdate {
  uint64_t mPanelID {};
  float mU {};
  float mV {};
  TouchState mState {TouchState::NotNear};
  // Mouse wheel movement, in the units of WM_MOUSEWHEEL (120 per notch);
  // positive scrolls up, i.e. the content moves down with the fingers
  int32_t mWheel {0};

  constexpr bool operator==(const CursorUpdate&) const noexcept = default;
};

// Hand poses that enable interaction
enum class Gesture : uint8_t {
  None,
  // Index finger out: tap to click
  Point,
  // Index and middle fingers out: touch and drag to scroll
  Scroll,
};

// At most 3 per frame: ending a click, leaving the previous panel, then the
// current state
struct CursorUpdates {
  std::array<CursorUpdate, 3> mItems {};
  uint8_t mCount {0};

  void Push(const CursorUpdate& update) noexcept {
    if (mCount < mItems.size()) {
      mItems[mCount++] = update;
    }
  }
  [[nodiscard]]
  auto begin() const noexcept {
    return mItems.begin();
  }
  [[nodiscard]]
  auto end() const noexcept {
    return mItems.begin() + mCount;
  }
  [[nodiscard]]
  std::size_t size() const noexcept {
    return mCount;
  }
};

enum class Phase : uint8_t {
  // Not near a panel, or not pointing
  Idle,
  // Showing the cursor
  Hovering,
  // Clicking
  Touching,
  // The fingertip reached a panel without approaching it from the front, or
  // was not aimed at it; ignored until it pulls back
  Blocked,
  // A tap just ended; the cursor stays, but tapping again needs the fingertip
  // to come back out first
  Releasing,
};

struct HandDiagnostics {
  bool mIsTracked {false};
  FingerCurls mCurls {};
  // The pose this frame, and the one that's enabled interaction
  Gesture mGesture {Gesture::None};
  Gesture mArmedGesture {Gesture::None};
  bool mIsPointing {false};
  bool mIsArmed {false};
  Phase mPhase {Phase::Idle};
  std::optional<uint64_t> mNearestPanelID;
  PanelProjection mNearest {};
  float mAimCosine {};
  // Estimated fingertip tracking noise (standard deviation, meters), and the
  // pull-back distance that ends a quick tap because of it
  float mTrackingNoise {};
  float mTapRetractDistance {};
  // How the most recent touch came in: sideways movement per unit inward, in
  // the panel's directions (+u = right, +v = down)
  Vector2 mApproachSlope {};
  bool mIsHold {false};
  bool mIsDragging {false};
};

struct FrameResult {
  CursorUpdates mCursorUpdates {};
  std::array<float, HandCount> mHandVisibility {};
  std::array<HandDiagnostics, HandCount> mDiagnostics {};
  std::optional<Hand> mActiveHand;
};

// Smooths noisy positions: little smoothing when moving fast (low lag), more
// smoothing when nearly still (low jitter).
// See https://gery.casiez.net/1euro/
class OneEuroFilter final {
 public:
  [[nodiscard]]
  Vector3 Update(
    const Vector3& value,
    float dt,
    float minCutoff,
    float beta,
    float derivativeCutoff);
  void Reset() noexcept;

 private:
  bool mHaveValue {false};
  Vector3 mValue {};
  Vector3 mDerivative {};
};

class Tracker final {
 public:
  explicit Tracker(const Settings& = {});

  // `panels` are in the order they're
  // drawn; where they overlap equally, the later one (drawn on top) wins.
  // Hands with non-finite joint data (NaN or infinity) are treated as not
  // tracked.
  [[nodiscard]]
  FrameResult Update(
    double nowSeconds,
    const std::array<HandJoints, HandCount>& hands,
    std::span<const Panel> panels,
    const Vector3& viewerPosition);

  // Releases any touch and hides the hands; the result contains any cursor
  // updates needed to leave the current panel
  [[nodiscard]]
  FrameResult Reset();

  [[nodiscard]]
  const Settings& GetSettings() const noexcept {
    return mSettings;
  }

  void SetTouchOffsetUp(float meters) noexcept {
    // Ignore a setting that isn't a number
    mSettings.mTouchOffsetUp = std::isfinite(meters) ? meters : 0.0f;
  }

 private:
  struct HandState {
    // The pose this frame (with hysteresis), and since when
    Gesture mGesture {Gesture::None};
    std::optional<double> mGestureSince;
    // The pose that has enabled interaction, and since when it hasn't
    // been held
    Gesture mArmedGesture {Gesture::None};
    std::optional<double> mNotArmedGestureSince;
    // mArmedGesture != None
    bool mIsArmed {false};
    // The pose when the current touch started
    Gesture mTouchGesture {Gesture::None};
    // Which point is tracked: the index fingertip, or between the index and
    // middle fingertips
    Gesture mContactPointFor {Gesture::None};

    // Scrolling
    float mScrollV {};
    float mScrollTravel {};
    bool mIsScrolling {false};
    int32_t mPendingWheel {0};

    OneEuroFilter mCursorFilter;
    OneEuroFilter mTouchFilter;
    OneEuroFilter mDepthFilter;

    // Tracking noise: from the last two depth-filtered fingertips
    std::array<Vector3, 2> mRecentDepthTips {};
    int mRecentDepthTipCount {0};
    float mTrackingNoise {0.0005f};
    double mTouchStartTime {};

    // The last few depth-filtered fingertips, and when, for the direction
    // they're moving in
    std::array<Vector3, 3> mApproachTips {};
    std::array<double, 3> mApproachTimes {};
    int mApproachCount {0};

    Phase mPhase {Phase::Idle};
    uint64_t mPanelID {};
    float mDistance {};
    Vector2 mUV {};
    Vector2 mTouchUV {};
    // Where the touch landed, on the panel
    Vector3 mTouchTip {};
    // The direction the touch came in: sideways movement per unit of
    // inward movement. The touch is tracked along this, so pushing further
    // in or pulling back out the way the finger came isn't a drag.
    Vector3 mApproachSlope {};
    // ... and in the panel's directions, for the log
    Vector2 mLastApproachSlope {};
    // The deepest the fingertip went during this tap
    float mDeepest {};
    // Long or moving touches; see Settings::mLongPressTime
    bool mIsHold {false};
    // How deep a hold is held; follows slow drift
    float mHoldDepth {};
    // Smoothed speeds while touching (m/s): sideways, and pulling back
    float mLateralSpeed {};
    float mRetractSpeed {};
    bool mIsDragging {false};
    double mDragStartTime {};
    // How far the pointer is from the fingertip (in UV) when a drag starts;
    // shrinks to nothing
    Vector2 mDragOffset {};

    // Last frame's lightly-smoothed fingertip, for finding exactly where it
    // crossed the touch distance
    std::optional<uint64_t> mPreviousPanelID;
    Vector3 mPreviousTouchTip {};
    float mPreviousDistance {};

    float mVisibility {0};
  };

  Settings mSettings;
  std::array<HandState, HandCount> mHands {};
  std::optional<double> mLastUpdate;
  std::optional<CursorUpdate> mLastSent;
  // Which hand `mLastSent` came from, and
  // which hand is pressing; only one hand can press at a time
  std::optional<std::size_t> mLastSentHand;
  std::optional<std::size_t> mTouchOwner;

  void UpdateHand(
    HandState&,
    HandDiagnostics&,
    double now,
    float dt,
    const HandJoints&,
    std::span<const Panel> panels,
    const Vector3& viewerPosition);

  // `desiredHand` is the hand `desired`
  // comes from
  void EmitCursor(
    FrameResult&,
    const std::optional<CursorUpdate>& desired,
    std::optional<std::size_t> desiredHand);
};

}// namespace OpenDesktopVR::HandInteraction
