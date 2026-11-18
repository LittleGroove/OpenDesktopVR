// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <algorithm>
#include <format>

namespace OpenDesktopVR::Settings {

namespace {

std::string PathForLog(const fs::path& path) {
  return StringUtil::Narrow(path.wstring());
}

Json DefaultsFor(const DocumentSpec& spec) {
  Json document = spec.mDefaults ? spec.mDefaults() : Json::object();
  if (!document.is_object()) {
    document = Json::object();
  }
  document[std::string {VersionKey}] = spec.mCurrentVersion;
  return document;
}

}// namespace

Store::Store(StoreOptions options) : mOptions(std::move(options)) {
  mWriter = std::thread([this] {
    ThreadUtil::SetCurrentThreadName(L"Settings writer");
    WriterMain();
  });
}

Store::~Store() {
  {
    std::scoped_lock lock(mMutex);
    mStopping = true;
  }
  mWake.notify_all();
  if (mWriter.joinable()) {
    mWriter.join();
  }
}

TimeUtil::TimePoint Store::DueTime(const Pending& pending) const noexcept {
  return std::min(pending.mLastRequest + mOptions.mDebounce, pending.mFirstRequest + mOptions.mMaxDelay);
}

void Store::Save(const fs::path& path, Json document) {
  const auto now = TimeUtil::Now();
  {
    std::scoped_lock lock(mMutex);
    ++mCounters.mSaveRequests;
    auto [it, inserted] = mPending.try_emplace(path);
    if (inserted) {
      it->second.mFirstRequest = now;
    }
    it->second.mLastRequest = now;
    it->second.mDocument = std::move(document);
  }
  mWake.notify_all();
}

void Store::Flush() {
  std::unique_lock lock(mMutex);
  if (mPending.empty() && !mWriting) {
    return;
  }
  ++mFlushRequests;
  mWake.notify_all();
  mIdle.wait(lock, [this] { return mPending.empty() && !mWriting; });
  --mFlushRequests;
}

bool Store::Delete(const fs::path& path) {
  Flush();
  if (!FileUtil::Exists(path)) {
    return true;
  }
  if (FileUtil::Remove(path)) {
    return true;
  }
  Log::Warning("Settings: couldn't delete {}", PathForLog(path));
  return false;
}

StoreCounters Store::Counters() const {
  std::scoped_lock lock(mMutex);
  return mCounters;
}

void Store::WriterMain() {
  std::unique_lock lock(mMutex);
  for (;;) {
    const auto now = TimeUtil::Now();
    const bool urgent = mFlushRequests > 0 || mStopping;
    auto due = mPending.end();
    auto nextDue = TimeUtil::TimePoint::max();
    for (auto it = mPending.begin(); it != mPending.end(); ++it) {
      const auto itDue = DueTime(it->second);
      if (urgent || itDue <= now) {
        due = it;
        break;
      }
      nextDue = std::min(nextDue, itDue);
    }

    if (due == mPending.end()) {
      if (mStopping) {
        break;
      }
      mIdle.notify_all();
      if (nextDue == TimeUtil::TimePoint::max()) {
        mWake.wait(lock);
      } else {
        mWake.wait_until(lock, nextDue);
      }
      continue;
    }

    const auto path = due->first;
    const auto document = std::move(due->second.mDocument);
    mPending.erase(due);
    mWriting = true;
    lock.unlock();
    // No lock held while touching the disk
    ThreadUtil::CatchAll("settings writer", [&] { WriteNow(path, document); });
    lock.lock();
    mWriting = false;
  }
  mIdle.notify_all();
}

void Store::WriteNow(const fs::path& path, const Json& document) {
  auto text = JsonUtil::Dump(document, mOptions.mIndent);
  text += '\n';
  const auto result = FileUtil::WriteAtomic(path, text, mOptions.mWrite);
  {
    std::scoped_lock lock(mMutex);
    ++mCounters.mDiskWrites;
    mCounters.mRenameRetries += static_cast<std::uint64_t>(result.mRenameRetries);
    if (result.mOutcome == FileUtil::WriteOutcome::InPlaceFallback) {
      ++mCounters.mInPlaceFallbacks;
    } else if (result.mOutcome == FileUtil::WriteOutcome::Failed) {
      ++mCounters.mFailedWrites;
    }
  }
  switch (result.mOutcome) {
    case FileUtil::WriteOutcome::Atomic:
      break;
    case FileUtil::WriteOutcome::InPlaceFallback:
      Log::Warning(
        "Settings: {} was saved in place (not atomically): the rename failed with error {}",
        PathForLog(path),
        result.mLastError);
      break;
    case FileUtil::WriteOutcome::Failed:
      Log::Error("Settings: couldn't save {} (error {})", PathForLog(path), result.mLastError);
      break;
  }
  mOnWritten.Emit(path, result.mOutcome);
}

LoadResult Store::Load(const DocumentSpec& spec) {
  Flush();// ST-3
  LoadResult result;
  const auto useDefaults = [&](LoadStatus status, std::string problem) {
    result.mDocument = DefaultsFor(spec);
    result.mStatus = status;
    result.mProblem = std::move(problem);
    if (!result.mProblem.empty()) {
      Log::Warning("Settings: {}", result.mProblem);
    }
    return result;
  };

  const auto text = FileUtil::ReadAll(spec.mPath);
  if (!text) {
    if (!FileUtil::Exists(spec.mPath)) {
      return useDefaults(LoadStatus::Missing, {});
    }
    return useDefaults(
      LoadStatus::Unreadable, std::format("couldn't read {}; using defaults", PathForLog(spec.mPath)));
  }

  auto parsed = JsonUtil::Parse(*text);
  if (!parsed || !parsed->is_object()) {
    result.mCopiedTo = FileUtil::CopyAside(spec.mPath, L"unreadable");
    return useDefaults(
      LoadStatus::Unreadable,
      std::format(
        "{} isn't valid settings ({}); kept as {}; using defaults",
        PathForLog(spec.mPath),
        text->empty() ? "empty" : (parsed ? "not an object" : "malformed JSON"),
        result.mCopiedTo ? PathForLog(*result.mCopiedTo) : "(copy failed)"));
  }

  Json document = std::move(*parsed);
  // A missing or broken version is treated as current: a hand edit shouldn't
  // trigger migrations
  const int version = JsonUtil::GetOr<int>(document, VersionKey, spec.mCurrentVersion);
  result.mFileVersion = version;

  if (version > spec.mCurrentVersion) {
    JsonUtil::FillMissing(document, DefaultsFor(spec));
    document[std::string {VersionKey}] = version;// keep the newer build's version
    result.mDocument = std::move(document);
    result.mStatus = LoadStatus::NewerVersion;
    Log::Info(
      "Settings: {} is from a newer version ({} > {}); unknown fields are kept",
      PathForLog(spec.mPath),
      version,
      spec.mCurrentVersion);
    return result;
  }

  if (version < spec.mCurrentVersion) {
    result.mCopiedTo = FileUtil::CopyAside(spec.mPath, std::format(L"v{}.bak", version));
    if (!result.mCopiedTo) {
      return useDefaults(
        LoadStatus::MigrationFailed,
        std::format("couldn't back up {} before migrating it; using defaults", PathForLog(spec.mPath)));
    }
    for (int v = version; v < spec.mCurrentVersion; ++v) {
      const auto migration = std::ranges::find(spec.mMigrations, v, &Migration::mFromVersion);
      bool ok = false;
      if (migration != spec.mMigrations.end() && migration->mApply) {
        ThreadUtil::CatchAll("settings migration", [&] { ok = migration->mApply(document); });
      }
      if (!ok) {
        return useDefaults(
          LoadStatus::MigrationFailed,
          std::format(
            "couldn't migrate {} from version {}; backup kept as {}; using defaults",
            PathForLog(spec.mPath),
            v,
            PathForLog(*result.mCopiedTo)));
      }
    }
    document[std::string {VersionKey}] = spec.mCurrentVersion;
    JsonUtil::FillMissing(document, DefaultsFor(spec));
    result.mDocument = document;
    result.mStatus = LoadStatus::Migrated;
    Save(spec.mPath, std::move(document));
    Log::Info(
      "Settings: migrated {} from version {} to {}",
      PathForLog(spec.mPath),
      version,
      spec.mCurrentVersion);
    return result;
  }

  JsonUtil::FillMissing(document, DefaultsFor(spec));
  result.mDocument = std::move(document);
  result.mStatus = LoadStatus::Loaded;
  return result;
}

}// namespace OpenDesktopVR::Settings
