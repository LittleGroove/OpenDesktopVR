// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Settings documents on disk (ADR-010; doc 09 "Settings storage"):
// - Save() is debounced (~300 ms after the last change, at most 2 s after
//   the first) and written by a background thread (ST-1).
// - Writes are atomic with retried renames and unique temp names, falling
//   back to an in-place write with a warning (ST-2).
// - Load() and Delete() flush pending writes first; so must anything that
//   reads or replaces the files (ST-3). Call Flush() for those.
// - Unreadable files never block startup: they're copied aside as
//   ".unreadable" and defaults are used (ST-4).
// - Old schema versions are backed up, then migrated; unknown fields survive
//   every round trip (ST-5).
//
// Don't call Flush, Load or Delete while holding a lock another thread
// needs: they wait for the writer thread (TH-1).
#pragma once

#include <OpenDesktopVR/Events.hpp>
#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace OpenDesktopVR::Settings {

using JsonUtil::Json;
namespace fs = std::filesystem;

// Every document stores its schema version under this key
inline constexpr std::string_view VersionKey = "schemaVersion";

struct Migration {
  int mFromVersion {0};
  // Upgrades a document at mFromVersion to mFromVersion + 1, in place.
  // Return false if it can't.
  std::function<bool(Json&)> mApply;
};

struct DocumentSpec {
  fs::path mPath;
  int mCurrentVersion {1};
  // A complete default document; VersionKey is added for you
  std::function<Json()> mDefaults;
  std::vector<Migration> mMigrations;
};

enum class LoadStatus {
  Loaded,
  Missing,        // no file: defaults
  Unreadable,     // couldn't read or parse: copied aside, defaults
  Migrated,       // upgraded from an older version (backup kept), saved
  MigrationFailed,// backup kept, defaults
  NewerVersion,   // written by a newer build: used as-is, unknown fields kept
};

struct LoadResult {
  Json mDocument;
  LoadStatus mStatus {LoadStatus::Loaded};
  int mFileVersion {0};
  std::optional<fs::path> mCopiedTo;// the ".unreadable" copy or the backup
  std::string mProblem;             // for logs and the "start with defaults" prompt
};

struct StoreOptions {
  std::chrono::milliseconds mDebounce {300};
  std::chrono::milliseconds mMaxDelay {2000};
  FileUtil::AtomicWriteOptions mWrite;
  int mIndent {2};
};

struct StoreCounters {
  std::uint64_t mSaveRequests {0};
  std::uint64_t mDiskWrites {0};
  std::uint64_t mRenameRetries {0};
  std::uint64_t mInPlaceFallbacks {0};
  std::uint64_t mFailedWrites {0};
};

class Store {
 public:
  explicit Store(StoreOptions options = {});
  ~Store();// flushes everything pending
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  LoadResult Load(const DocumentSpec& spec);

  // Queues `document` to be written to `path`; replaces anything already
  // queued for that path. Never touches the disk on the calling thread.
  void Save(const fs::path& path, Json document);

  // Returns once every queued write has reached the disk (or failed)
  void Flush();

  // Flushes, then deletes the file. False if it exists and can't be deleted.
  bool Delete(const fs::path& path);

  StoreCounters Counters() const;

  // Raised on the writer thread after each write, with its outcome
  Events::Event<fs::path, FileUtil::WriteOutcome>& OnWritten() noexcept {
    return mOnWritten;
  }

 private:
  struct Pending {
    Json mDocument;
    TimeUtil::TimePoint mFirstRequest;
    TimeUtil::TimePoint mLastRequest;
  };

  void WriterMain();
  void WriteNow(const fs::path& path, const Json& document);
  TimeUtil::TimePoint DueTime(const Pending& pending) const noexcept;

  const StoreOptions mOptions;
  mutable std::mutex mMutex;
  std::condition_variable mWake;
  std::condition_variable mIdle;
  std::map<fs::path, Pending> mPending;
  bool mWriting {false};
  int mFlushRequests {0};
  bool mStopping {false};
  StoreCounters mCounters;
  Events::Event<fs::path, FileUtil::WriteOutcome> mOnWritten;
  std::thread mWriter;// last: starts after everything above exists
};

}// namespace OpenDesktopVR::Settings
