// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
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
//   BeginInjection and EndInjection. Right after a press, the window is
//   made active on its own thread, if it isn't (ActivateWindow): Chromium
//   apps and Firefox only show a text field's caret in an active window
//   (M5b, ADR-036; ADR-034 had left it out, as it takes the real
//   foreground where Windows allows it, which the helper now gives back:
//   ADR-035). After the press, so the press's latency never includes it.
// - **Leaving** a panel lets go of a held button and clears hover (INJ-5);
//   destroying the injector posts any release still held (INJ-4).
// - **Double taps** on a window that wants double-clicks (CS_DBLCLKS) send
//   WM_LBUTTONDBLCLK, at the first tap's point: sent messages never become
//   double-clicks by themselves. A second tap counts if it starts within the
//   double-click time of the first one's *lift* and lands within 15 mm of it
//   on the panel; the first must have been a tap (lifted within that time,
//   without sliding). Decided on the hand thread, from the finger's own
//   timing (ADR-034, amended after round 11: finger taps are held 110-250 ms
//   and land 10-20 px apart, so a mouse's rule missed most of them).
// - **Logs** name the app: a message that timed out, the app not
//   responding, and when it answers again (with what was skipped).
// - **Keys** (M5a, doc 06 "Keyboard injection"): a whole key press goes
//   through the same worker, after any taps before it. The window is made
//   active on its own thread first (Firefox needs it); then the key goes to
//   the focused window on the app's thread, found when it's delivered
//   (INJ-7), or with none, to the control the last press went to; the
//   keyboard's modifiers are set on the app's thread; the characters arrive
//   with the key-down (INJ-9); blocked combinations never go (INJ-8). Keys
//   never merge; to an app that isn't responding they're skipped, like taps.
//   Where Windows lets the app take the real foreground when it activates,
//   the helper gives it back at once and at each injection's end (ADR-035).
#pragma once

#include <OpenDesktopVR/HandInteraction.hpp>
#include <OpenDesktopVR/WindowKeyInjection.hpp>

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
  // The panel's size in VR, in meters (0: unknown), for finger distances
  float mWidthMeters {0};
  float mHeightMeters {0};
};

struct Options {
  std::chrono::milliseconds mTimeout {250};
  double mHoverPerSecond {30};
  std::size_t mQueueCapacity {256};
  // A second tap this close to the first (each way, on the panel) is a
  // double tap, if it also starts soon enough after the first one's lift.
  // About a fingertip: fingers land 5-10 mm apart (round 11)
  float mDoubleTapMillimeters {15};
  // The same in the panel's pixels, if its size isn't known
  int mDoubleTapPixels {16};
};

struct Stats {
  uint64_t mPresses {0};
  uint64_t mDoubleClicks {0};
  uint64_t mSkippedPresses {0};// of mSkippedHung: presses (taps that didn't land)
  uint64_t mKeys {0};
  uint64_t mKeysBlocked {0};// INJ-8
  uint64_t mKeysSkipped {0};// of mSkippedHung: keys to an app not responding
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

  // One key press (down, its characters, up; or a repeat) into the target's
  // window. Never waits. May be called from another thread than Pointer()
  // (the keyboard's repeat): workers are made under a lock (INJ-7).
  void Key(const Target& target, const VirtualKeyStroke& stroke);

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
