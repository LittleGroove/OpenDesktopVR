// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace OpenDesktopVR::Log {

namespace {

struct Slot {
  std::atomic<std::size_t> mSequence {0};
  std::uint64_t mFileTime {0};
  std::uint32_t mThreadID {0};
  Level mLevel {Level::Info};
  std::uint16_t mLength {0};
  char mText[MaxMessageBytes] {};
};

// Bounded multi-producer, multi-consumer queue of fixed slots.
// The algorithm is Dmitry Vyukov's bounded MPMC queue (published at
// 1024cores.net under a BSD-2-Clause-style license; see THIRD_PARTY_NOTICES):
// each slot carries a sequence number that tells producers and consumers
// whose turn it is, so neither side ever takes a lock.
// The two positions sit on separate cache lines on purpose (C4324).
#pragma warning(push)
#pragma warning(disable : 4324)
class BoundedQueue {
 public:
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0, "capacity must be a power of two");
  static constexpr std::size_t Mask = QueueCapacity - 1;

  BoundedQueue() {
    for (std::size_t i = 0; i < QueueCapacity; ++i) {
      mSlots[i].mSequence.store(i, std::memory_order_relaxed);
    }
  }

  template <class Fill>
  bool TryPush(Fill&& fill) noexcept {
    auto position = mEnqueue.load(std::memory_order_relaxed);
    for (;;) {
      auto& slot = mSlots[position & Mask];
      const auto sequence = slot.mSequence.load(std::memory_order_acquire);
      const auto difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
      if (difference == 0) {
        if (mEnqueue.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
          fill(slot);
          slot.mSequence.store(position + 1, std::memory_order_release);
          return true;
        }
      } else if (difference < 0) {
        return false;// full
      } else {
        position = mEnqueue.load(std::memory_order_relaxed);
      }
    }
  }

  template <class Read>
  bool TryPop(Read&& read) noexcept {
    auto position = mDequeue.load(std::memory_order_relaxed);
    for (;;) {
      auto& slot = mSlots[position & Mask];
      const auto sequence = slot.mSequence.load(std::memory_order_acquire);
      const auto difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1);
      if (difference == 0) {
        if (mDequeue.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
          read(slot);
          slot.mSequence.store(position + QueueCapacity, std::memory_order_release);
          return true;
        }
      } else if (difference < 0) {
        return false;// empty
      } else {
        position = mDequeue.load(std::memory_order_relaxed);
      }
    }
  }

 private:
  alignas(64) std::atomic<std::size_t> mEnqueue {0};
  alignas(64) std::atomic<std::size_t> mDequeue {0};
  std::array<Slot, QueueCapacity> mSlots;
};
#pragma warning(pop)

struct State {
  BoundedQueue mQueue;
  std::atomic<std::uint64_t> mQueued {0};
  std::atomic<std::uint64_t> mWritten {0};
  std::atomic<std::uint64_t> mDropped {0};
  std::atomic<std::uint64_t> mDropNoticePending {0};
  std::atomic<std::uint64_t> mTruncated {0};
  std::atomic<Level> mMinimumLevel {Level::Info};
  HANDLE mWake {CreateEventW(nullptr, FALSE, FALSE, nullptr)};

  // Start/Stop only; never taken by Write
  std::mutex mControl;
  std::thread mThread;
  HANDLE mThreadDone {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  std::atomic<bool> mStopRequested {false};
  std::atomic<bool> mRunning {false};
  Config mConfig;

  Events::Event<Entry> mSinks;
};

// Never destroyed: no cleanup at process exit (XR-17), and messages from
// static destructors still have somewhere to go
State& GetState() {
  static State* sState = new State();
  return *sState;
}

std::uint64_t NowFileTime() noexcept {
  FILETIME ft {};
  GetSystemTimePreciseAsFileTime(&ft);
  return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void Enqueue(Level level, std::string_view text, std::uint64_t fileTime, std::uint32_t threadID) noexcept {
  auto& state = GetState();
  const auto fill = [&](Slot& slot) noexcept {
    slot.mFileTime = fileTime;
    slot.mThreadID = threadID;
    slot.mLevel = level;
    const auto fitted = StringUtil::TruncateUtf8(text, MaxMessageBytes);
    std::memcpy(slot.mText, fitted.data(), fitted.size());
    slot.mLength = static_cast<std::uint16_t>(fitted.size());
  };
  // Full: drop the oldest entry and try again (a few times, then drop this one)
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (state.mQueue.TryPush(fill)) {
      state.mQueued.fetch_add(1, std::memory_order_relaxed);
      if (state.mWake) {
        SetEvent(state.mWake);
      }
      return;
    }
    if (state.mQueue.TryPop([](Slot&) noexcept {})) {
      state.mDropped.fetch_add(1, std::memory_order_relaxed);
      state.mDropNoticePending.fetch_add(1, std::memory_order_relaxed);
    }
  }
  state.mDropped.fetch_add(1, std::memory_order_relaxed);
  state.mDropNoticePending.fetch_add(1, std::memory_order_relaxed);
}

class Writer {
 public:
  explicit Writer(const Config& config) : mConfig(config) {
    if (mConfig.mFile.empty()) {
      return;
    }
    FileUtil::CreateFolders(mConfig.mFile.parent_path());
    if (const auto size = FileUtil::Size(mConfig.mFile); size && *size > mConfig.mRotateBytes) {
      auto previous = mConfig.mFile;
      previous.replace_extension(L".previous.log");
      MoveFileExW(mConfig.mFile.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    mFile = CreateFileW(
      mConfig.mFile.c_str(),
      FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  }
  ~Writer() {
    if (mFile != INVALID_HANDLE_VALUE) {
      CloseHandle(mFile);
    }
  }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  void Add(const Entry& entry) {
    auto& state = GetState();
    const auto line = FormatLine(entry);
    mBatch += line;
    mBatch += "\r\n";
    if (mConfig.mDebuggerOutput && IsDebuggerPresent()) {
      OutputDebugStringW(StringUtil::Widen(line + "\n").c_str());
    }
    state.mSinks.Emit(entry);
    state.mWritten.fetch_add(1, std::memory_order_relaxed);
  }

  void Flush() {
    if (mBatch.empty()) {
      return;
    }
    if (mFile != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(mFile, mBatch.data(), static_cast<DWORD>(mBatch.size()), &written, nullptr);
    }
    mBatch.clear();
  }

 private:
  Config mConfig;
  HANDLE mFile {INVALID_HANDLE_VALUE};
  std::string mBatch;
};

void Drain(Writer& writer) {
  auto& state = GetState();
  // The dropped entries were the oldest, so the notice goes first
  if (const auto dropped = state.mDropNoticePending.exchange(0); dropped > 0) {
    const auto text = std::format("{} log messages dropped (the log queue was full)", dropped);
    writer.Add({NowFileTime(), GetCurrentThreadId(), Level::Warning, text});
  }
  Slot copy;
  for (int i = 0; i < 4 * static_cast<int>(QueueCapacity); ++i) {
    const bool popped = state.mQueue.TryPop([&](Slot& slot) noexcept {
      copy.mFileTime = slot.mFileTime;
      copy.mThreadID = slot.mThreadID;
      copy.mLevel = slot.mLevel;
      copy.mLength = slot.mLength;
      std::memcpy(copy.mText, slot.mText, slot.mLength);
    });
    if (!popped) {
      break;
    }
    writer.Add({copy.mFileTime, copy.mThreadID, copy.mLevel, {copy.mText, copy.mLength}});
  }
  writer.Flush();
}

void ThreadMain(Config config) {
  ThreadUtil::SetCurrentThreadName(L"Log writer");
  auto& state = GetState();
  ThreadUtil::CatchAll("log writer", [&] {
    Writer writer {config};
    for (;;) {
      if (state.mWake) {
        WaitForSingleObject(state.mWake, INFINITE);
      } else {
        Sleep(100);
      }
      const bool stopping = state.mStopRequested.load();
      Drain(writer);
      if (stopping) {
        break;
      }
    }
  });
  SetEvent(state.mThreadDone);
}

}// namespace

std::string_view LevelName(Level level) noexcept {
  switch (level) {
    case Level::Debug:
      return "DEBUG";
    case Level::Info:
      return "INFO ";
    case Level::Warning:
      return "WARN ";
    case Level::Error:
      return "ERROR";
  }
  return "?    ";
}

std::string FormatLine(const Entry& entry) {
  FILETIME utc {static_cast<DWORD>(entry.mFileTime), static_cast<DWORD>(entry.mFileTime >> 32)};
  FILETIME local {};
  SYSTEMTIME t {};
  FileTimeToLocalFileTime(&utc, &local);
  FileTimeToSystemTime(&local, &t);
  return std::format(
    "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{:6}] {} {}",
    t.wYear,
    t.wMonth,
    t.wDay,
    t.wHour,
    t.wMinute,
    t.wSecond,
    t.wMilliseconds,
    entry.mThreadID,
    LevelName(entry.mLevel),
    entry.mText);
}

void Start(const Config& config) noexcept {
  auto& state = GetState();
  try {
    std::scoped_lock lock(state.mControl);
    if (state.mRunning.load()) {
      return;
    }
    state.mConfig = config;
    state.mMinimumLevel.store(config.mMinimumLevel);
    state.mStopRequested.store(false);
    ResetEvent(state.mThreadDone);
    state.mThread = std::thread(ThreadMain, config);
    state.mRunning.store(true);
  } catch (...) {
    // No thread: messages stay queued (bounded) and nothing blocks
  }
  Info(
    "{} {} log started (process {})",
    StringUtil::Narrow(Product::DisplayName),
    Product::Version,
    GetCurrentProcessId());
}

void Stop(StopMode mode) noexcept {
  auto& state = GetState();
  if (mode == StopMode::ProcessExit) {
    // Threads may already be gone and may hold locks: touch nothing (LG-1)
    state.mStopRequested.store(true);
    if (state.mWake) {
      SetEvent(state.mWake);
    }
    return;
  }
  // try-lock: a thread killed at exit may hold the control lock (LG-1)
  std::unique_lock lock(state.mControl, std::try_to_lock);
  if (!lock.owns_lock() || !state.mRunning.load()) {
    return;
  }
  state.mStopRequested.store(true);
  if (state.mWake) {
    SetEvent(state.mWake);
  }
  const auto waitMs = static_cast<DWORD>(std::max<long long>(0, state.mConfig.mStopWait.count()));
  if (state.mThread.joinable()) {
    if (WaitForSingleObject(state.mThreadDone, waitMs) == WAIT_OBJECT_0) {
      state.mThread.join();
    } else {
      state.mThread.detach();
    }
  }
  state.mRunning.store(false);
}

bool IsRunning() noexcept {
  return GetState().mRunning.load();
}

void SetMinimumLevel(Level level) noexcept {
  GetState().mMinimumLevel.store(level);
}

Level MinimumLevel() noexcept {
  return GetState().mMinimumLevel.load(std::memory_order_relaxed);
}

void Write(Level level, std::string_view text) noexcept {
  if (level < MinimumLevel()) {
    return;
  }
  Detail::WriteFormatted(level, text, text.size() > MaxMessageBytes);
}

void Detail::WriteFormatted(Level level, std::string_view text, bool truncated) noexcept {
  if (truncated) {
    GetState().mTruncated.fetch_add(1, std::memory_order_relaxed);
  }
  Enqueue(level, text, NowFileTime(), GetCurrentThreadId());
}

Counters GetCounters() noexcept {
  const auto& state = GetState();
  return {
    .mQueued = state.mQueued.load(),
    .mWritten = state.mWritten.load(),
    .mDropped = state.mDropped.load(),
    .mTruncated = state.mTruncated.load(),
  };
}

Events::Subscription AddSink(std::function<void(const Entry&)> sink) {
  return GetState().mSinks.Subscribe(std::move(sink));
}

}// namespace OpenDesktopVR::Log

namespace OpenDesktopVR::ThreadUtil {

void ReportEscapedException(std::string_view where) noexcept {
  Log::Error("An exception escaped {}: {}", where, DescribeCurrentException());
}

}// namespace OpenDesktopVR::ThreadUtil
