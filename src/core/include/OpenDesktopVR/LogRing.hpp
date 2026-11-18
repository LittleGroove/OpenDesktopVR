// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The newest log lines, for the host's log viewer and the debug bundle
// (LG-3, doc 09 "Diagnostics page").
//
// - Bounded: at most Capacity lines, each at most Log::MaxMessageBytes plus
//   its time and level; older lines drop off the front.
// - Only Info and above: Debug detail (per-frame, hover) stays out of it.
// - Fed on the log thread through a sink, read from any thread. Reading
//   copies, so the UI never holds the lock while drawing.
//
//   LogRing ring;                       // collecting from now on
//   if (ring.Version() != shown) { lines = ring.Lines(); ... }
#pragma once

#include <OpenDesktopVR/Events.hpp>
#include <OpenDesktopVR/Log.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace OpenDesktopVR::Log {

class LogRing {
 public:
  static constexpr std::size_t DefaultCapacity = 1000;

  struct Line {
    Level mLevel {Level::Info};
    std::string mText;// FormatLine's text: time, thread, level, message
  };

  // Collects from now on (until destroyed); `subscribe` false for tests,
  // which call Add themselves
  explicit LogRing(std::size_t capacity = DefaultCapacity, bool subscribe = true);
  ~LogRing() = default;
  LogRing(const LogRing&) = delete;
  LogRing& operator=(const LogRing&) = delete;

  // The lines kept, oldest first
  std::vector<Line> Lines() const;
  // All of them as text, one a line ("\r\n"), for Copy and the debug bundle
  std::string Text() const;
  // Changes whenever a line is added: redraw only then (RN-12)
  uint64_t Version() const noexcept;
  std::size_t Capacity() const noexcept;
  // Lines that dropped off the front so far
  uint64_t Dropped() const noexcept;

  // The sink (the log thread); public for tests
  void Add(const Entry& entry) noexcept;

 private:
  struct State;
  // Shared with the sink: a line being added as the ring goes still lands
  // somewhere valid
  std::shared_ptr<State> mState;
  Events::Subscription mSubscription;// last: removed first
};

}// namespace OpenDesktopVR::Log
