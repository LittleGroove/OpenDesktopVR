// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/LogRing.hpp>

#include <algorithm>

namespace OpenDesktopVR::Log {

struct LogRing::State {
  explicit State(std::size_t capacity) : mCapacity(std::max<std::size_t>(capacity, 1)) {
    mLines.reserve(mCapacity);
  }

  void Add(const Entry& entry) noexcept {
    if (entry.mLevel < Level::Info) {
      return;// LG-3: no Debug detail
    }
    try {
      Line line {entry.mLevel, FormatLine(entry)};
      std::scoped_lock lock(mMutex);
      if (mLines.size() < mCapacity) {
        mLines.push_back(std::move(line));
      } else {
        mLines[mNext] = std::move(line);
        mNext = (mNext + 1) % mCapacity;
        ++mDropped;
      }
      ++mVersion;
    } catch (...) {
      // Out of memory: the line is lost, logging goes on
    }
  }

  const std::size_t mCapacity;
  mutable std::mutex mMutex;
  std::vector<Line> mLines;// a ring once full: mNext is the oldest
  std::size_t mNext {0};
  uint64_t mVersion {0};
  uint64_t mDropped {0};
};

LogRing::LogRing(std::size_t capacity, bool subscribe) : mState(std::make_shared<State>(capacity)) {
  if (subscribe) {
    mSubscription = AddSink([state = mState](const Entry& entry) { state->Add(entry); });
  }
}

void LogRing::Add(const Entry& entry) noexcept {
  mState->Add(entry);
}

std::size_t LogRing::Capacity() const noexcept {
  return mState->mCapacity;
}

std::vector<LogRing::Line> LogRing::Lines() const {
  std::scoped_lock lock(mState->mMutex);
  const auto& ring = mState->mLines;
  std::vector<Line> lines;
  lines.reserve(ring.size());
  for (std::size_t i = 0; i < ring.size(); ++i) {
    lines.push_back(ring[(mState->mNext + i) % ring.size()]);
  }
  return lines;
}

std::string LogRing::Text() const {
  std::string text;
  for (const auto& line: Lines()) {
    text += line.mText;
    text += "\r\n";
  }
  return text;
}

uint64_t LogRing::Version() const noexcept {
  std::scoped_lock lock(mState->mMutex);
  return mState->mVersion;
}

uint64_t LogRing::Dropped() const noexcept {
  std::scoped_lock lock(mState->mMutex);
  return mState->mDropped;
}

}// namespace OpenDesktopVR::Log
