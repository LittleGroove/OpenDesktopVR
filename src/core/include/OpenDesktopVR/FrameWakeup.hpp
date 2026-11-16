// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// The host app renders frames on a timer. Other threads (e.g. when a
// captured window has a new frame) can ask for the next frame to happen
// sooner, instead of waiting for the timer; this keeps what's shown in VR as
// fresh as possible.
#pragma once

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace OpenDesktopVR::FrameWakeup {

namespace Detail {
using Clock = std::chrono::steady_clock;

// Early frames are never closer together than this. Frames are rendered on
// the UI thread, so this was lowered from 4ms (up to 250 frames a second
// while captured windows were animating, which made the app window sluggish)
// to at most 125 frames a second.
inline constexpr auto MinFrameInterval = std::chrono::milliseconds(8);

struct State {
  std::mutex mMutex;
  HANDLE mTimer {nullptr};
  Clock::time_point mLastFrameStart {};
  Clock::time_point mScheduled {Clock::time_point::max()};
};

inline State& GetState() {
  static State state;
  return state;
}

// Caller must hold the lock
inline void Arm(State& state, Clock::time_point when) {
  state.mScheduled = when;
  using HundredNanoseconds = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;
  const auto relative = std::chrono::duration_cast<HundredNanoseconds>(
                          when - Clock::now())
                          .count();
  // Negative values are relative; 0 is 'now'
  const LARGE_INTEGER dueTime {.QuadPart = -std::max<int64_t>(relative, 0)};
  SetWaitableTimer(state.mTimer, &dueTime, 0, nullptr, nullptr, 0);
}
}// namespace Detail

// A timer for the frame loop. High resolution where Windows has it (10 1803
// and later): a default waitable timer fires only on the system tick
// (15.6 ms), so "again in 2 ms" took a whole tick, and captured video drew
// at about 17 frames a second against a cap of 30 (test round 5). Null on
// failure.
inline HANDLE CreateTimer(bool* highResolution = nullptr) noexcept {
  constexpr DWORD HighResolution = 0x00000002;// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, HighResolution, TIMER_ALL_ACCESS);
  if (highResolution) {
    *highResolution = timer != nullptr;
  }
  return timer ? timer : CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

// The frame loop's waitable timer; nullptr when the loop stops
inline void SetTimer(HANDLE timer) {
  auto& state = Detail::GetState();
  std::scoped_lock lock(state.mMutex);
  state.mTimer = timer;
  state.mScheduled = Detail::Clock::time_point::max();
}

// Called by the frame loop when the timer fires
inline void FrameStarted() {
  auto& state = Detail::GetState();
  std::scoped_lock lock(state.mMutex);
  state.mLastFrameStart = Detail::Clock::now();
  state.mScheduled = Detail::Clock::time_point::max();
}

// Called by the frame loop for its regular next frame; an earlier request
// that's already scheduled is kept
inline void ScheduleNextFrame(Detail::Clock::time_point when) {
  auto& state = Detail::GetState();
  std::scoped_lock lock(state.mMutex);
  if (state.mTimer && when < state.mScheduled) {
    Detail::Arm(state, when);
  }
}

// Any thread: something changed, so render as soon as reasonable
inline void RequestFrameSoon() {
  auto& state = Detail::GetState();
  std::scoped_lock lock(state.mMutex);
  if (!state.mTimer) {
    return;
  }
  const auto when = std::max(
    Detail::Clock::now(), state.mLastFrameStart + Detail::MinFrameInterval);
  if (when < state.mScheduled) {
    Detail::Arm(state, when);
  }
}

}// namespace OpenDesktopVR::FrameWakeup
