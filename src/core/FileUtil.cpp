// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/FileUtil.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <thread>

namespace OpenDesktopVR::FileUtil {

namespace {

struct HandleCloser {
  HANDLE mHandle {INVALID_HANDLE_VALUE};
  ~HandleCloser() {
    if (mHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(mHandle);
    }
  }
};

bool WriteAllTo(HANDLE file, std::string_view contents) noexcept {
  std::size_t written = 0;
  while (written < contents.size()) {
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size() - written, 1u << 30));
    DWORD done = 0;
    if (!WriteFile(file, contents.data() + written, chunk, &done, nullptr) || done == 0) {
      return false;
    }
    written += done;
  }
  return true;
}

// Creates (or truncates) `path` and writes `contents`, flushed to disk.
// Readers (antivirus, indexers) may keep the file open; other writers may not.
DWORD WriteWholeFile(const fs::path& path, std::string_view contents, DWORD disposition) noexcept {
  HandleCloser file {CreateFileW(
    path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.mHandle == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }
  if (!WriteAllTo(file.mHandle, contents) || !FlushFileBuffers(file.mHandle)) {
    return GetLastError();
  }
  return ERROR_SUCCESS;
}

}// namespace

std::optional<std::string> ReadAll(const fs::path& path) noexcept {
  try {
    HandleCloser file {CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr)};
    if (file.mHandle == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file.mHandle, &size) || size.QuadPart < 0 || size.QuadPart > (1ll << 30)) {
      return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t read = 0;
    while (read < contents.size()) {
      DWORD done = 0;
      const auto chunk = static_cast<DWORD>(contents.size() - read);
      if (!ReadFile(file.mHandle, contents.data() + read, chunk, &done, nullptr)) {
        return std::nullopt;
      }
      if (done == 0) {
        contents.resize(read);// the file shrank while we read it
        break;
      }
      read += done;
    }
    return contents;
  } catch (...) {
    return std::nullopt;
  }
}

fs::path UniqueTempPathFor(const fs::path& path) {
  static std::atomic<std::uint64_t> sCounter {0};
  auto name = path.filename().wstring();
  name += std::format(
    L".{}-{}-{}.tmp", GetCurrentProcessId(), GetCurrentThreadId(), sCounter.fetch_add(1, std::memory_order_relaxed));
  return path.parent_path() / name;
}

bool IsTransientShareError(unsigned long win32Error) noexcept {
  switch (win32Error) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_ACCESS_DENIED:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
      return true;
    default:
      return false;
  }
}

WriteResult WriteAtomic(const fs::path& path, std::string_view contents, const AtomicWriteOptions& options) noexcept {
  WriteResult result;
  try {
    if (path.has_parent_path()) {
      CreateFolders(path.parent_path());
    }
    const auto temp = UniqueTempPathFor(path);
    if (const auto error = WriteWholeFile(temp, contents, CREATE_ALWAYS); error != ERROR_SUCCESS) {
      result.mLastError = error;
      Remove(temp);
      return result;
    }

    const int attempts = std::max(1, options.mRenameAttempts);
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if (attempt > 0) {
        ++result.mRenameRetries;
        std::this_thread::sleep_for(options.mRenameRetryDelay);
      }
      if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.mOutcome = WriteOutcome::Atomic;
        return result;
      }
      result.mLastError = GetLastError();
      if (!IsTransientShareError(result.mLastError)) {
        break;
      }
    }
    Remove(temp);

    if (options.mAllowInPlaceFallback) {
      // CREATE_ALWAYS truncates the existing file in place, keeping its identity
      if (const auto error = WriteWholeFile(path, contents, CREATE_ALWAYS); error == ERROR_SUCCESS) {
        result.mOutcome = WriteOutcome::InPlaceFallback;
      } else {
        result.mLastError = error;
      }
    }
    return result;
  } catch (...) {
    result.mOutcome = WriteOutcome::Failed;
    return result;
  }
}

std::optional<fs::path> CopyAside(const fs::path& path, std::wstring_view suffix) noexcept {
  try {
    const auto base = path.wstring() + L"." + std::wstring {suffix};
    fs::path target {base};
    for (int n = 1; Exists(target) && n < 1000; ++n) {
      target = std::format(L"{}.{}", base, n);
    }
    std::error_code ec;
    if (!fs::copy_file(path, target, fs::copy_options::none, ec) || ec) {
      return std::nullopt;
    }
    return target;
  } catch (...) {
    return std::nullopt;
  }
}

bool Exists(const fs::path& path) noexcept {
  std::error_code ec;
  return fs::exists(path, ec) && !ec;
}

bool Remove(const fs::path& path) noexcept {
  std::error_code ec;
  return fs::remove(path, ec) && !ec;
}

bool CreateFolders(const fs::path& folder) noexcept {
  std::error_code ec;
  fs::create_directories(folder, ec);
  return !ec && fs::is_directory(folder, ec);
}

std::optional<std::uintmax_t> Size(const fs::path& path) noexcept {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return size;
}

}// namespace OpenDesktopVR::FileUtil
