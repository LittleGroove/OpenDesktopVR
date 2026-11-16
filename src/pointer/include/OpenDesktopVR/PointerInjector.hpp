// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Taps, drags, hover and scrolling from the hands, delivered to captured
// windows as window messages, while the game keeps the real foreground,
// mouse and keyboard (doc 06; R1-R4; INJ-1 to INJ-6; M4b).
//
// - **One worker per captured window,** with its own bounded queue (INJ-1,
//   TH-5). Callers never wait: the hand thread only queues.
// - **Moves merge;** buttons and wheel never merge or drop. Hover moves go
//   out at most 30 a second (doc 05).
// - **Each message is sent with a timeout** (about 250 ms; INJ-2), from the
//   worker, never a UI thread; a timeout isn't retried.
// - **Hung windows** (IsHungAppWindow): releases and the end of injection
//   are posted; moves, presses and wheel are skipped until it recovers, so it
//   gets no stuck button and no burst of late clicks (INJ-3).
// - **Every press is preceded by a move to its exact point,** with the
//   buttons as they were before (INJ-6); a release ends where the finger
//   let go (the tracker keeps a tap's release where it went down).
// - **Targets:** the deepest enabled, visible, non-transparent child under
//   the point; during a press, the window that got it (capture). Points map
//   from the panel through the part of the client area it shows, in the
//   window's own DPI.
// - **The app helper's input mode** (if loaded): each event is bracketed by
//   BeginInjection and EndInjection. A press doesn't make the window active
//   (ActivateWindow): measured, that takes the real foreground whenever
//   Windows allows it (ADR-034).
// - **Leaving** a panel lets go of a held button and clears hover (INJ-5);
//   destroying the injector posts any release still held (INJ-4).
// - **Double taps** on a window that wants double-clicks (CS_DBLCLKS) send
//   WM_LBUTTONDBLCLK, at the first tap's point: sent messages never become
//   double-clicks by themselves.
#pragma once

#include <OpenDesktopVR/HandInteraction.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace OpenDesktopVR::Pointer {

// Where a panel's input goes
struct Target {
  HWND mWindow {nullptr};// the captured top-level window
  // The part of its client area the panel shows, and the client area's
  // size, in the captured frame's pixels
  RECT mShown {};
  SIZE mClient {};
};

struct Options {
  std::chrono::milliseconds mTimeout {250};
  double mHoverPerSecond {30};
  std::size_t mQueueCapacity {256};
  // A second tap this close (pixels, each way) and this soon is a double
  // tap; fingers land a few pixels apart, so wider than a mouse's
  int mDoubleTapPixels {16};
};

struct Stats {
  uint64_t mPresses {0};
  uint64_t mDoubleClicks {0};
  uint64_t mReleases {0};
  uint64_t mMoves {0};
  uint64_t mMovesMerged {0};
  uint64_t mWheelMessages {0};
  uint64_t mTimeouts {0};
  uint64_t mSkippedHung {0};
  uint64_t mPostedReleases {0};
  uint64_t mWindowsGone {0};
  uint64_t mDropped {0};// the queue was full (never expected)
  double mLongestWaitMs {0};// queued to delivered, presses and releases
};

class Injector {
 public:
  explicit Injector(Options options = {});
  // Stops the workers and posts any release still held (INJ-4)
  ~Injector();
  Injector(const Injector&) = delete;
  Injector& operator=(const Injector&) = delete;

  // One pointer update from the hands, in order, from one thread (the hand
  // thread). `target` null: the panel takes no input or has gone. Never
  // waits.
  void Pointer(uint64_t panelID, const Target* target, const HandInteraction::CursorUpdate& update);

  Stats GetStats() const;
  // A stats line, if anything happened since the last one
  void LogStats(const char* title);

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

// For tests: what a point on the panel (u, v from the top left) is in a
// client area `clientWidth` x `clientHeight` (the window's own units)
POINT ClientPoint(const Target& target, float u, float v, LONG clientWidth, LONG clientHeight) noexcept;

}// namespace OpenDesktopVR::Pointer
