// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The host's hand thread (doc 02 "Threads", "Data flow: a tap"; doc 05):
// - wakes on the layer's tracking event, never on a timer while nothing is
//   happening; reads every new sample in order (a reader more than the
//   ring's depth behind skips the oldest);
// - runs the imported interaction engine (`HandInteraction::Tracker`) on
//   the panels the hands may touch, nearest-aimed first as the layer draws
//   them, and hands each pointer change to a PointerSink (M4b: the
//   injector);
// - tells the layer how much of each hand to draw (the pointing pose fades
//   it in; "show my hand while pointing" off hides it);
// - no sample for 250 ms, or samples 250 ms apart, releases any touch and
//   starts fresh (XR-19); shorter gaps don't end a drag;
// - logs every touch (where, how it came in, hold and drag, how long) and a
//   stats line a minute (doc 05 "Tuning workflow", LG-2);
// - while the keyboard panel shows (M5b, ADR-036): runs the imported
//   `TypingTracker` on it and hands each result to a TypingSink; the
//   keyboard isn't a pointing target, a hand near it doesn't point, and
//   both hands are drawn (K2).
// Every callback is caught (TH-8).
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/HandInteraction.hpp>
#include <OpenDesktopVR/HandTyping.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace OpenDesktopVR::Host {

// What the user chose for hands ("hands" in settings.json)
struct HandSettings {
  // "Tap and scroll panels with your hands"
  bool mEnabled {true};
  // "Show my hand while pointing" (doc 05 "Hand visual")
  bool mShowWhilePointing {true};
  // Moves taps up the panel by this much, for a personal offset (doc 05)
  float mTapOffsetUpMillimeters {0};
  // "Reverse two-finger scrolling": natural (content follows the fingers)
  // is the default
  bool mReverseScroll {false};

  static constexpr float MaxTapOffsetMillimeters = 30;
  bool operator==(const HandSettings&) const noexcept = default;
};

// A panel the hands may touch, as the layer shows it (LOCAL space)
struct TouchPanel {
  uint64_t mID {0};
  std::wstring mName;
  HandInteraction::Panel mGeometry {};
  bool mTouch {true}; // tap, hold and drag
  bool mScroll {true};// two-finger scroll
  // The virtual keyboard: typed on, never pointed at (ADR-036)
  bool mKeyboard {false};
  // Where input goes (window panels); nullopt for content that takes none
  std::optional<Render::InputTarget> mTarget;
  bool operator==(const TouchPanel& other) const noexcept;
};
using TouchPanels = std::vector<TouchPanel>;

// The hand thread's view of the shared block: samples in, hand state out
class TrackingChannel {
 public:
  virtual ~TrackingChannel() = default;
  // Set after each sample (an auto-reset event)
  virtual void* Event() const noexcept = 0;
  // The newest sample's index (0: none yet)
  virtual uint64_t Written() const noexcept = 0;
  virtual bool Read(uint64_t index, SharedAbi::TrackingSample& out) const noexcept = 0;
  virtual void WriteHandState(const SharedAbi::HandState& state) noexcept = 0;
};

// Where pointer changes go. Called on the hand thread, in order; must not
// block (the injector queues them).
class PointerSink {
 public:
  virtual ~PointerSink() = default;
  // `panel` is the touched or hovered panel's entry (nullptr if it has
  // gone); `update` is in its UV (0..1 from the top left)
  virtual void Pointer(const TouchPanel* panel, const HandInteraction::CursorUpdate& update) = 0;
};

// Where typing goes (the keyboard service). Called on the hand thread, in
// order; must not block.
class TypingSink {
 public:
  virtual ~TypingSink() = default;
  // Each sample while the keyboard shows. `appeared`: the first since it
  // showed, or since tracking stopped: press counts start afresh (VK-4)
  virtual void Typing(const TouchPanel& keyboard, const HandInteraction::TypingResult& result, bool appeared) = 0;
  // The keyboard went, or tracking stopped: let go of every key
  virtual void TypingStopped() = 0;
  // Every time the hand thread runs while the keyboard shows (samples or
  // not): key repeat
  virtual void Tick() = 0;
};

struct HandServiceStats {
  uint64_t mSamples {0};
  uint64_t mSkipped {0};// fell out of the ring before being read
  uint64_t mTouches {0};
  uint64_t mTaps {0};
  uint64_t mHolds {0};
  uint64_t mDrags {0};
  uint64_t mScrollTouches {0};
  int64_t mWheel {0};// total wheel movement (120 per notch)
  uint64_t mResets {0};// tracking gaps that let go of a touch
  uint64_t mStateWrites {0};
};

struct HandServiceOptions {
  std::chrono::milliseconds mGap {250};// XR-19
  std::chrono::seconds mStatsPeriod {60};
};

class HandService {
 public:
  // `panels` returns the panels as last published (any thread). `sink` may
  // be null (touches are logged only). All must outlive the service.
  HandService(
    TrackingChannel& channel,
    std::function<std::shared_ptr<const TouchPanels>()> panels,
    PointerSink* sink,
    HandServiceOptions options = {},
    TypingSink* typing = nullptr);
  ~HandService();
  HandService(const HandService&) = delete;
  HandService& operator=(const HandService&) = delete;

  void Start();
  // Lets go of any touch (posted to the sink), then stops; idempotent
  void Stop() noexcept;

  // Any thread; applied before the next sample
  void SetSettings(const HandSettings& settings);

  HandServiceStats Stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

}// namespace OpenDesktopVR::Host
