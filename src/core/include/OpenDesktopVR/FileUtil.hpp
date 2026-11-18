// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// File helpers. All of them use the non-throwing filesystem calls (doc 02,
// rule 8): a missing folder, a locked file or a full disk is a result, never
// an exception.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace OpenDesktopVR::FileUtil {

namespace fs = std::filesystem;

// The whole file as bytes; nullopt if it can't be opened or read
std::optional<std::string> ReadAll(const fs::path& path) noexcept;

struct AtomicWriteOptions {
  // ST-2: antivirus and indexers hold files briefly, so retry the rename
  int mRenameAttempts {3};
  std::chrono::milliseconds mRenameRetryDelay {10};
  // ST-2: if every rename fails, write in place (not atomic, but saved)
  bool mAllowInPlaceFallback {true};
};

enum class WriteOutcome {
  Atomic,           // temp file + flush + rename
  InPlaceFallback,  // the rename kept failing; written in place
  Failed,
};

struct WriteResult {
  WriteOutcome mOutcome {WriteOutcome::Failed};
  int mRenameRetries {0};
  unsigned long mLastError {0};// Win32 error code of the last failure
};

// Writes `contents` to `path` atomically (ST-1, ST-2):
// write a uniquely named temp file next to it, FlushFileBuffers, then
// MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH), retrying the rename on
// sharing, access and lock violations. Creates the parent folder.
WriteResult WriteAtomic(
  const fs::path& path,
  std::string_view contents,
  const AtomicWriteOptions& options = {}) noexcept;

// A temp-file name unique to this process, thread and call (ST-2), in the
// same folder as `path`
fs::path UniqueTempPathFor(const fs::path& path);

// Copies `path` to a sibling named "<name>.<suffix>" (or, if that exists,
// "<name>.<suffix>.<n>"), leaving the original in place. Returns the copy's
// path, or nullopt if it couldn't be made. Used to keep unreadable files and
// pre-migration backups (ST-4, ST-5).
std::optional<fs::path> CopyAside(const fs::path& path, std::wstring_view suffix) noexcept;

bool Exists(const fs::path& path) noexcept;
bool Remove(const fs::path& path) noexcept;
bool CreateFolders(const fs::path& folder) noexcept;
std::optional<std::uintmax_t> Size(const fs::path& path) noexcept;

// Sharing, access and lock violations: worth retrying after a short delay
bool IsTransientShareError(unsigned long win32Error) noexcept;

}// namespace OpenDesktopVR::FileUtil
