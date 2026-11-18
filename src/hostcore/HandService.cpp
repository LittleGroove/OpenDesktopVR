// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <mutex>
#include <numbers>
#include <string>
#include <optional>
#include <thread>

namespace OpenDesktopVR::Host {

using namespace std::chrono_literals;
using HandInteraction::CursorUpdate;
using HandInteraction::FrameResult;
using HandInteraction::Gesture;
using HandInteraction::HandJoints;
using HandInteraction::Phase;
using HandInteraction::TouchState;
using HandInteraction::Vector3;

bool TouchPanel::operator==(const TouchPanel& other) const noexcept {
  const auto& a = mGeometry;
  const auto& b = other.mGeometry;
  return mID == other.mID && mTouch == other.mTouch && mScroll == other.mScroll && mKeyboard == other.mKeyboard
    && a.mPosition == b.mPosition
    && a.mOrientation == b.mOrientation && a.mSize == b.mSize && mName == other.mName && mTarget == other.mTarget;
}

namespace {

const char* HandName(std::size_t hand) {
  return hand == 0 ? "left" : "right";
}

double QpcMilliseconds(int64_t from, int64_t to) {
  static const double frequency = [] {
    LARGE_INTEGER value {};
    QueryPerformanceFrequency(&value);
    return static_cast<double>(value.QuadPart);
  }();
  return static_cast<double>(to - from) * 1000.0 / frequency;
}

int64_t QpcNow() {
  LARGE_INTEGER value {};
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

}// namespace

struct HandService::Impl {
  TrackingChannel& mChannel;
  std::function<std::shared_ptr<const TouchPanels>()> mPanelsSource;
  PointerSink* mSink;
  HandServiceOptions mOptions;
  TypingSink* mTypingSink;

  std::thread mThread;
  HANDLE mStop {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE mWake {CreateEventW(nullptr, FALSE, FALSE, nullptr)};

  std::mutex mSettingsMutex;
  HandSettings mPendingSettings;
  bool mSettingsChanged {true};

  // The hand thread's own state
  HandSettings mSettings;
  HandInteraction::Tracker mTracker;
  uint64_t mLastRead {0};
  std::optional<int64_t> mLastDisplayTime;
  TimeUtil::TimePoint mLastSampleAt {};
  Vector3 mViewer {};
  bool mActive {false};// something to let go of if tracking stops
  std::shared_ptr<const TouchPanels> mPanels;
  std::vector<HandInteraction::Panel> mGeometry;
  std::vector<const TouchPanel*> mGeometryEntries;
  // The keyboard panel, while it shows (ADR-036)
  const TouchPanel* mKeyboard {nullptr};
  HandInteraction::TypingTracker mTyping;
  bool mTypingStarted {false};// the sink has had a result since it appeared
  bool mTypingActive {false};// a finger is near or pressing
  // Reused each sample
  std::vector<std::size_t> mOrder;
  std::vector<float> mDistances;
  std::vector<HandInteraction::Panel> mSorted;
  std::array<float, 2> mVisibility {};
  uint32_t mFlags {0};
  bool mStateWritten {false};

  // Touch log (doc 05 "Tuning workflow")
  struct Touch {
    bool mActive {false};
    double mStart {0};
    uint64_t mPanelID {0};
    Gesture mGesture {Gesture::None};
    bool mHold {false};
    bool mDrag {false};
    int64_t mWheel {0};
  };
  std::array<Touch, 2> mTouches {};
  double mNow {0};// the newest sample's display time, in seconds

  // XR-19 for one hand, while samples keep coming: a touching hand whose
  // tracking is lost (fingers hidden for a few frames) is held where it was
  // last tracked for up to the gap, then let go (doc 03: "hand tracking off
  // or lost"). The imported tracker lets go at once, so this is bridged
  // here; found in M4c.
  struct Held {
    HandJoints mJoints;// the last tracked pose, while touching
    std::optional<double> mLostSince;// display time, seconds
  };
  std::array<Held, 2> mHeld {};
  std::array<bool, 2> mLostTooLong {};// let go for that: said in the touch log

  mutable std::mutex mStatsMutex;
  HandServiceStats mStats;
  // Per stats line
  uint64_t mPeriodSamples {0};
  uint64_t mPeriodSkipped {0};
  std::array<uint64_t, 2> mPeriodTracked {};
  uint64_t mPeriodTouches {0};
  double mLatencySumMs {0};
  double mLatencyMaxMs {0};
  TimeUtil::TimePoint mNextStats {};

  Impl(TrackingChannel& channel, std::function<std::shared_ptr<const TouchPanels>()> panels, PointerSink* sink,
    HandServiceOptions options, TypingSink* typing)
    : mChannel(channel), mPanelsSource(std::move(panels)), mSink(sink), mOptions(options), mTypingSink(typing) {
  }
  ~Impl() {
    CloseHandle(mStop);
    CloseHandle(mWake);
  }

  void Main();
  void ApplySettings();
  void ReadSamples();
  void Process(const SharedAbi::TrackingSample& sample);
  void RefreshPanels();
  void Route(const FrameResult& result);
  // Typing on the keyboard, if it shows; which hands are near it
  std::array<bool, 2> Type(const std::array<HandJoints, 2>& hands);
  void StopTyping();
  void LogTouches(const FrameResult& result);
  void EndTouch(std::size_t hand, const char* why);
  // `gap`: tracking stopped (counted when it lets go of a touch)
  void Reset(const char* why, bool gap = true);
  void WriteState();
  // Every stats period; and once at exit, so the last part isn't lost
  void MaybeWriteStats(TimeUtil::TimePoint now, bool exiting = false);
  const TouchPanel* Entry(uint64_t panelID) const;
  template <class Change>
  void Count(Change&& change) {
    std::scoped_lock lock(mStatsMutex);
    change(mStats);
  }
};

HandService::HandService(
  TrackingChannel& channel,
  std::function<std::shared_ptr<const TouchPanels>()> panels,
  PointerSink* sink,
  HandServiceOptions options,
  TypingSink* typing)
  : mImpl(std::make_unique<Impl>(channel, std::move(panels), sink, options, typing)) {
}

HandService::~HandService() {
  Stop();
}

void HandService::Start() {
  if (mImpl->mThread.joinable()) {
    return;
  }
  ResetEvent(mImpl->mStop);
  mImpl->mThread = std::thread([impl = mImpl.get()] {
    ThreadUtil::SetCurrentThreadName(L"Hands");
    ThreadUtil::CatchAll("hand thread", [impl] { impl->Main(); });
  });
}

void HandService::Stop() noexcept {
  if (!mImpl->mThread.joinable()) {
    return;
  }
  SetEvent(mImpl->mStop);
  mImpl->mThread.join();
}

void HandService::SetSettings(const HandSettings& settings) {
  {
    std::scoped_lock lock(mImpl->mSettingsMutex);
    mImpl->mPendingSettings = settings;
    mImpl->mSettingsChanged = true;
  }
  SetEvent(mImpl->mWake);
}

HandServiceStats HandService::Stats() const noexcept {
  std::scoped_lock lock(mImpl->mStatsMutex);
  return mImpl->mStats;
}

void HandService::Impl::Main() {
  ApplySettings();
  mNextStats = TimeUtil::Now() + mOptions.mStatsPeriod;
  for (;;) {
    // Nothing to let go of and nothing to report: sleep until the layer
    // sends a sample (RN-1's "no work, no wake-ups", for the hand thread)
    DWORD timeout = INFINITE;
    if (mActive) {
      timeout = 50;
    } else if (mPeriodSamples > 0) {
      const auto untilStats = std::chrono::duration_cast<std::chrono::milliseconds>(mNextStats - TimeUtil::Now());
      timeout = static_cast<DWORD>(std::max<int64_t>(0, untilStats.count()));
    }
    const HANDLE handles[] {mStop, mWake, static_cast<HANDLE>(mChannel.Event())};
    const DWORD count = handles[2] ? 3 : 2;
    const auto woke = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) {
      break;
    }
    ThreadUtil::CatchAll("hand thread step", [&] {
      ApplySettings();
      ReadSamples();
      if (mTypingSink && mTypingStarted) {
        ThreadUtil::CatchAll("typing sink", [&] { mTypingSink->Tick(); });
      }
      const auto now = TimeUtil::Now();
      // XR-19: no samples for a while lets go
      if (mActive && now - mLastSampleAt > mOptions.mGap) {
        Reset("no tracking for 250 ms");
      }
      MaybeWriteStats(now);
    });
  }
  // Exiting: let go of anything held, and stop the layer's samples
  ThreadUtil::CatchAll("hand thread exit", [&] {
    Reset("the host is stopping", false);
    mFlags = 0;
    WriteState();
    MaybeWriteStats(TimeUtil::Now(), true);
  });
}

void HandService::Impl::ApplySettings() {
  HandSettings settings;
  {
    std::scoped_lock lock(mSettingsMutex);
    if (!mSettingsChanged) {
      return;
    }
    mSettingsChanged = false;
    settings = mPendingSettings;
  }
  if (!std::isfinite(settings.mTapOffsetUpMillimeters)) {
    settings.mTapOffsetUpMillimeters = 0;
  }
  settings.mTapOffsetUpMillimeters = std::clamp(
    settings.mTapOffsetUpMillimeters, -HandSettings::MaxTapOffsetMillimeters, HandSettings::MaxTapOffsetMillimeters);
  const bool turnedOff = mSettings.mEnabled && !settings.mEnabled;
  mSettings = settings;
  mTracker.SetTouchOffsetUp(settings.mTapOffsetUpMillimeters / 1000.0f);
  mTyping.SetTouchOffsetUp(settings.mTapOffsetUpMillimeters / 1000.0f);
  if (turnedOff) {
    Reset("hands were turned off", false);
  }
  mFlags = settings.mEnabled ? SharedAbi::HandFlags::WantsSamples : 0;
  if (!settings.mShowWhilePointing) {
    mVisibility = {};
  }
  WriteState();
}

void HandService::Impl::ReadSamples() {
  const auto written = mChannel.Written();
  if (written < mLastRead) {
    mLastRead = 0;// a new block: start over
  }
  if (written == mLastRead) {
    return;
  }
  uint64_t first = mLastRead + 1;
  if (written - mLastRead > SharedAbi::TrackingSlots) {
    const auto skipped = (written - SharedAbi::TrackingSlots) - mLastRead;
    mPeriodSkipped += skipped;
    Count([&](auto& stats) { stats.mSkipped += skipped; });
    first = written - SharedAbi::TrackingSlots + 1;
  }
  SharedAbi::TrackingSample sample {};
  for (auto index = first; index <= written; ++index) {
    if (!mChannel.Read(index, sample)) {
      ++mPeriodSkipped;
      Count([](auto& stats) { ++stats.mSkipped; });
      continue;
    }
    Process(sample);
  }
  mLastRead = written;
}

void HandService::Impl::RefreshPanels() {
  auto panels = mPanelsSource ? mPanelsSource() : nullptr;
  if (panels == mPanels && !mGeometry.empty()) {
    return;
  }
  mPanels = std::move(panels);
  mGeometry.clear();
  mGeometryEntries.clear();
  mKeyboard = nullptr;
  if (!mPanels) {
    return;
  }
  for (const auto& panel: *mPanels) {
    if (panel.mKeyboard) {
      mKeyboard = &panel;// typed on, never pointed at
      continue;
    }
    if (panel.mTouch || panel.mScroll) {
      mGeometry.push_back(panel.mGeometry);
      mGeometryEntries.push_back(&panel);
    }
  }
}

const TouchPanel* HandService::Impl::Entry(uint64_t panelID) const {
  if (!mPanels) {
    return nullptr;
  }
  for (const auto& panel: *mPanels) {
    if (panel.mID == panelID) {
      return &panel;
    }
  }
  return nullptr;
}

void HandService::Impl::Process(const SharedAbi::TrackingSample& sample) {
  const auto now = TimeUtil::Now();
  mLastSampleAt = now;
  ++mPeriodSamples;
  Count([](auto& stats) { ++stats.mSamples; });
  const double latency = QpcMilliseconds(sample.mWrittenAt, QpcNow());
  if (std::isfinite(latency) && latency >= 0 && latency < 10'000) {
    mLatencySumMs += latency;
    mLatencyMaxMs = std::max(mLatencyMaxMs, latency);
  }
  if (!mSettings.mEnabled) {
    return;
  }
  // Samples far apart in time: the layer had no panels or tracking for a
  // while. Start fresh rather than continue an old touch (XR-19).
  const auto gapNs = std::chrono::duration_cast<std::chrono::nanoseconds>(mOptions.mGap).count();
  if (mLastDisplayTime && (sample.mDisplayTime < *mLastDisplayTime || sample.mDisplayTime - *mLastDisplayTime > gapNs)) {
    Reset("a tracking gap");
  }
  mLastDisplayTime = sample.mDisplayTime;
  mNow = static_cast<double>(sample.mDisplayTime) / 1e9;

  if (sample.mFlags & SharedAbi::SampleFlags::HeadValid) {
    const Vector3 head {sample.mHead[0], sample.mHead[1], sample.mHead[2]};
    if (std::isfinite(head.x) && std::isfinite(head.y) && std::isfinite(head.z)) {
      mViewer = head;
    }
  }
  std::array<HandJoints, 2> hands {};
  for (std::size_t h = 0; h < 2; ++h) {
    const auto& in = sample.mHands[h];
    auto& out = hands[h];
    out.mIsTracked = in.mTracked == 1;// the tracker drops non-finite hands itself
    if (!out.mIsTracked) {
      continue;
    }
    ++mPeriodTracked[h];
    for (std::size_t j = 0; j < SharedAbi::HandJointCount; ++j) {
      const auto& p = in.mJoints[j].mPose;
      out.mJoints[j] = {{p[0], p[1], p[2]}, {p[3], p[4], p[5], p[6]}, in.mJoints[j].mRadius};
    }
  }
  // Typing on the keyboard, with the hands as tracked (its tracker bridges
  // its own gaps); a hand near it doesn't point (doc 05)
  RefreshPanels();
  const auto nearKeyboard = Type(hands);

  const double gapSeconds = std::chrono::duration<double>(mOptions.mGap).count();
  for (std::size_t h = 0; h < 2; ++h) {
    auto& held = mHeld[h];
    if (hands[h].mIsTracked) {
      held.mJoints = hands[h];
      held.mLostSince.reset();
      continue;
    }
    if (mTouches[h].mActive && held.mJoints.mIsTracked) {
      if (!held.mLostSince) {
        held.mLostSince = mNow;
      }
      if (mNow - *held.mLostSince <= gapSeconds) {
        hands[h] = held.mJoints;
        continue;
      }
      mLostTooLong[h] = true;
      Count([](auto& stats) { ++stats.mResets; });
    }
    // Not touching, or lost too long: let go; when it's back it starts fresh
    held = {};
  }
  for (std::size_t h = 0; h < 2; ++h) {
    if (nearKeyboard[h]) {
      hands[h] = {};
    }
  }

  // Back to front from the head, as the layer draws them: where panels
  // overlap, the nearer (drawn later) wins
  mOrder.resize(mGeometry.size());
  mDistances.resize(mGeometry.size());
  for (std::size_t i = 0; i < mGeometry.size(); ++i) {
    mOrder[i] = i;
    mDistances[i] = HandInteraction::DistanceToPanel(mViewer, mGeometry[i]);
  }
  std::stable_sort(mOrder.begin(), mOrder.end(), [&](std::size_t a, std::size_t b) { return mDistances[a] > mDistances[b]; });
  mSorted.clear();
  for (const auto i: mOrder) {
    mSorted.push_back(mGeometry[i]);
  }

  const auto result = mTracker.Update(mNow, hands, mSorted, mViewer);
  Route(result);
  LogTouches(result);
  mVisibility = mSettings.mShowWhilePointing ? result.mHandVisibility : std::array<float, 2> {};
  if (mKeyboard) {
    mVisibility = {1.0f, 1.0f};// both hands while the keyboard shows (K2)
  }
  mActive = mVisibility[0] > 0 || mVisibility[1] > 0 || mTypingActive
    || std::any_of(result.mDiagnostics.begin(), result.mDiagnostics.end(), [](const auto& d) { return d.mPhase != Phase::Idle; });
  WriteState();
}

std::array<bool, 2> HandService::Impl::Type(const std::array<HandJoints, 2>& hands) {
  if (!mKeyboard) {
    StopTyping();
    return {};
  }
  const auto result = mTyping.Update(mNow, hands, mKeyboard->mGeometry, mViewer);
  mTypingActive = std::any_of(result.mFingers.begin(), result.mFingers.end(),
    [](const auto& finger) { return finger.mState != HandInteraction::TypingFingerState::NotNear; });
  const bool appeared = !std::exchange(mTypingStarted, true);
  if (mTypingSink) {
    ThreadUtil::CatchAll("typing sink", [&] { mTypingSink->Typing(*mKeyboard, result, appeared); });
  }
  return result.mHandNearKeyboard;
}

void HandService::Impl::StopTyping() {
  if (!mTypingStarted) {
    return;
  }
  mTypingStarted = false;
  mTypingActive = false;
  (void)mTyping.Reset();
  if (mTypingSink) {
    ThreadUtil::CatchAll("typing sink", [&] { mTypingSink->TypingStopped(); });
  }
}

void HandService::Impl::Route(const FrameResult& result) {
  for (auto update: result.mCursorUpdates) {
    if (update.mWheel != 0) {
      if (mSettings.mReverseScroll) {
        update.mWheel = -update.mWheel;
      }
      Count([&](auto& stats) { stats.mWheel += update.mWheel; });
      if (result.mActiveHand) {
        mTouches[static_cast<std::size_t>(*result.mActiveHand)].mWheel += update.mWheel;
      }
    }
    if (mSink) {
      const auto* entry = Entry(update.mPanelID);
      ThreadUtil::CatchAll("pointer sink", [&] { mSink->Pointer(entry, update); });
    }
  }
}

void HandService::Impl::LogTouches(const FrameResult& result) {
  for (std::size_t h = 0; h < 2; ++h) {
    const auto& diagnostics = result.mDiagnostics[h];
    auto& touch = mTouches[h];
    const bool touching = diagnostics.mPhase == Phase::Touching;
    if (touching && !touch.mActive) {
      touch = {.mActive = true, .mStart = mNow, .mPanelID = diagnostics.mNearestPanelID.value_or(0),
        .mGesture = diagnostics.mArmedGesture};
      const auto* entry = Entry(touch.mPanelID);
      const float aimDegrees
        = std::acos(std::clamp(diagnostics.mAimCosine, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
      Log::Info(
        "Touch: {} hand, {}, on '{}' at ({:.3f}, {:.3f}); came in at slope ({:.2f}, {:.2f}), {:.0f} degrees off straight; "
        "noise {:.2f} mm, a tap ends {:.1f} mm back",
        HandName(h),
        touch.mGesture == Gesture::Scroll ? "two fingers" : "pointing",
        entry ? StringUtil::Narrow(entry->mName) : std::string {"?"},
        diagnostics.mNearest.mU,
        diagnostics.mNearest.mV,
        diagnostics.mApproachSlope.x,
        diagnostics.mApproachSlope.y,
        aimDegrees,
        diagnostics.mTrackingNoise * 1000.0f,
        diagnostics.mTapRetractDistance * 1000.0f);
      ++mPeriodTouches;
      Count([&](auto& stats) {
        ++stats.mTouches;
        stats.mScrollTouches += touch.mGesture == Gesture::Scroll ? 1 : 0;
      });
      continue;
    }
    if (!touch.mActive) {
      continue;
    }
    if (touching) {
      if (diagnostics.mIsHold && !touch.mHold) {
        touch.mHold = true;
        Log::Info("Touch: {} hand holds ({:.0f} ms)", HandName(h), (mNow - touch.mStart) * 1000.0);
      }
      if (diagnostics.mIsDragging && !touch.mDrag) {
        touch.mDrag = true;
        Log::Info("Touch: {} hand drags ({:.0f} ms)", HandName(h), (mNow - touch.mStart) * 1000.0);
      }
      continue;
    }
    EndTouch(h, mLostTooLong[h] ? "its tracking was lost for 250 ms" : nullptr);
    mLostTooLong[h] = false;
  }
}

void HandService::Impl::EndTouch(std::size_t hand, const char* why) {
  auto& touch = mTouches[hand];
  if (!touch.mActive) {
    return;
  }
  const char* kind = touch.mGesture == Gesture::Scroll ? "scroll" : touch.mDrag ? "drag" : touch.mHold ? "hold" : "tap";
  Log::Info("Touch: {} hand let go: a {} of {:.0f} ms{}{}{}",
    HandName(hand),
    kind,
    (mNow - touch.mStart) * 1000.0,
    touch.mWheel != 0 ? std::format(", {:.1f} wheel notches", static_cast<double>(touch.mWheel) / 120.0) : std::string {},
    why ? ", " : "",
    why ? why : "");
  Count([&](auto& stats) {
    if (touch.mGesture != Gesture::Scroll) {
      stats.mDrags += touch.mDrag ? 1 : 0;
      stats.mHolds += (!touch.mDrag && touch.mHold) ? 1 : 0;
      stats.mTaps += (!touch.mDrag && !touch.mHold) ? 1 : 0;
    }
  });
  touch = {};
}

void HandService::Impl::Reset(const char* why, bool gap) {
  const bool wasActive = mActive;
  const bool wasTouching = mTouches[0].mActive || mTouches[1].mActive;
  const auto result = mTracker.Reset();
  Route(result);
  for (std::size_t h = 0; h < 2; ++h) {
    EndTouch(h, why);
  }
  StopTyping();// keys let go; the next sample starts a new baseline (VK-4)
  mHeld = {};
  mLostTooLong = {};
  mLastDisplayTime.reset();
  mVisibility = {};
  mActive = false;
  if (gap && wasTouching) {
    Count([](auto& stats) { ++stats.mResets; });
  }
  if (wasActive) {
    WriteState();
  }
}

void HandService::Impl::WriteState() {
  SharedAbi::HandState state {};
  state.mVisibility[0] = mVisibility[0];
  state.mVisibility[1] = mVisibility[1];
  state.mFlags = mFlags;
  state.mAnsweredSample = mLastRead;
  mChannel.WriteHandState(state);
  Count([](auto& stats) { ++stats.mStateWrites; });
}

void HandService::Impl::MaybeWriteStats(TimeUtil::TimePoint now, bool exiting) {
  if (now < mNextStats && !exiting) {
    return;
  }
  mNextStats = now + mOptions.mStatsPeriod;
  if (mPeriodSamples == 0) {
    return;
  }
  const auto stats = [&] {
    std::scoped_lock lock(mStatsMutex);
    return mStats;
  }();
  const double samples = static_cast<double>(mPeriodSamples);
  Log::Info(
    "Hands ({}): {} samples ({} skipped); hands tracked: left {:.0f}%, right {:.0f}%; {} touches; sample to host "
    "{:.2f} ms on average, at most {:.2f} ms; in all: {} taps, {} holds, {} drags, {:.1f} wheel notches, {} let go "
    "after tracking stopped",
    exiting ? std::string {"to exit"} : std::format("{} s", mOptions.mStatsPeriod.count()),
    mPeriodSamples,
    mPeriodSkipped,
    100.0 * static_cast<double>(mPeriodTracked[0]) / samples,
    100.0 * static_cast<double>(mPeriodTracked[1]) / samples,
    mPeriodTouches,
    mLatencySumMs / samples,
    mLatencyMaxMs,
    stats.mTaps,
    stats.mHolds,
    stats.mDrags,
    static_cast<double>(stats.mWheel) / 120.0,
    stats.mResets);
  mPeriodSamples = mPeriodSkipped = mPeriodTouches = 0;
  mPeriodTracked = {};
  mLatencySumMs = mLatencyMaxMs = 0;
}

}// namespace OpenDesktopVR::Host
