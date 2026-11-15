// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Logging that is safe inside the game process (LG-1, XR-3, XR-4):
// - Writing never blocks and never allocates: the message is formatted into
//   a stack buffer and copied into a fixed slot of a bounded lock-free
//   queue (256 slots).
// - When the queue is full the oldest entry is dropped, and an
//   "N messages dropped" line appears once space returns.
// - Over-long messages are truncated (never split mid-character).
// - A background thread drains the queue to the log file, the debugger and
//   any extra sinks (the host's log viewer, tests).
// - Stop waits at most 250 ms; at process exit it doesn't wait at all.
//
// Log::Info("Panel {} shown at {:.2f} m", id, distance);
#pragma once

#include <OpenDesktopVR/Events.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <string_view>

namespace OpenDesktopVR::Log {

enum class Level : std::uint8_t { Debug, Info, Warning, Error };

std::string_view LevelName(Level level) noexcept;

inline constexpr std::size_t QueueCapacity = 256;
inline constexpr std::size_t MaxMessageBytes = 480;

// What sinks receive, on the log thread
struct Entry {
  std::uint64_t mFileTime {0};// UTC, as a FILETIME
  std::uint32_t mThreadID {0};
  Level mLevel {Level::Info};
  std::string_view mText;
};

struct Config {
  // Empty: no file. The folder is created. An existing file over
  // mRotateBytes is kept as "<name>.previous.log".
  std::filesystem::path mFile;
  std::uintmax_t mRotateBytes {4u << 20};
  bool mDebuggerOutput {true};
  Level mMinimumLevel {Level::Info};
  std::chrono::milliseconds mStopWait {250};
};

// Starts the log thread. Messages written before Start are kept (up to the
// queue's capacity) and written once it runs. Restartable after Stop.
void Start(const Config& config) noexcept;

enum class StopMode {
  Drain,      // write what's queued, waiting at most Config::mStopWait
  ProcessExit,// don't wait for anything (XR-17: no cleanup at exit)
};
void Stop(StopMode mode = StopMode::Drain) noexcept;

bool IsRunning() noexcept;
void SetMinimumLevel(Level level) noexcept;
Level MinimumLevel() noexcept;

// Never blocks, never allocates, never throws
void Write(Level level, std::string_view text) noexcept;

struct Counters {
  std::uint64_t mQueued {0};
  std::uint64_t mWritten {0};
  std::uint64_t mDropped {0};
  std::uint64_t mTruncated {0};
};
Counters GetCounters() noexcept;

// Extra outputs, called on the log thread for every entry
Events::Subscription AddSink(std::function<void(const Entry&)> sink);

// Formats an entry as one line, without a line ending:
// "2026-11-15 05:01:02.123 [  1234] INFO  text" (local time)
std::string FormatLine(const Entry& entry);

namespace Detail {
void WriteFormatted(Level level, std::string_view text, bool truncated) noexcept;

template <class... Args>
void Format(Level level, std::format_string<Args...> format, Args&&... args) noexcept {
  if (level < MinimumLevel()) {
    return;
  }
  try {
    std::array<char, MaxMessageBytes> buffer;
    const auto result = std::format_to_n(buffer.data(), buffer.size(), format, std::forward<Args>(args)...);
    const auto size = static_cast<std::size_t>(std::min<std::ptrdiff_t>(result.size, buffer.size()));
    const bool truncated = static_cast<std::size_t>(result.size) > buffer.size();
    std::string_view text {buffer.data(), size};
    if (truncated) {
      text = StringUtil::TrimIncompleteUtf8Tail(text);
    }
    WriteFormatted(level, text, truncated);
  } catch (...) {
    WriteFormatted(level, "(a log message failed to format)", false);
  }
}
}// namespace Detail

template <class... Args>
void Debug(std::format_string<Args...> format, Args&&... args) noexcept {
  Detail::Format(Level::Debug, format, std::forward<Args>(args)...);
}
template <class... Args>
void Info(std::format_string<Args...> format, Args&&... args) noexcept {
  Detail::Format(Level::Info, format, std::forward<Args>(args)...);
}
template <class... Args>
void Warning(std::format_string<Args...> format, Args&&... args) noexcept {
  Detail::Format(Level::Warning, format, std::forward<Args>(args)...);
}
template <class... Args>
void Error(std::format_string<Args...> format, Args&&... args) noexcept {
  Detail::Format(Level::Error, format, std::forward<Args>(args)...);
}

}// namespace OpenDesktopVR::Log
