// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// A small HTTPS client on Windows' own WinHTTP, for update checks (ADR-045).
// - HTTPS only: plain HTTP is refused, except to 127.0.0.1 when a test asks
//   (its fake GitHub);
// - redirects are followed (GitHub sends downloads elsewhere), never from
//   HTTPS to HTTP;
// - every answer is bounded in size and time; the system's proxy settings
//   are used;
// - it sends a User-Agent naming the program and its version, nothing else
//   about the user.
// Blocking: call it from a worker thread, never the UI thread.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Update {

struct HttpOptions {
  std::uint64_t mMaxBytes {1u << 20};
  // Tests only: http://127.0.0.1 allowed
  bool mAllowLocalHttp {false};
  std::vector<std::wstring> mHeaders;// "Name: value"
  std::chrono::milliseconds mTimeout {30'000};
};

struct HttpResult {
  bool mOk {false};// a complete answer, status 200
  unsigned mStatus {0};
  std::string mBody;// Get only
  std::uint64_t mBytes {0};
  std::string mError;// why not, for the log and the user
};

HttpResult Get(std::string_view url, const HttpOptions& options) noexcept;

// Streams the answer into `file` (replacing it). `progress` gets the bytes
// so far and the total (0 if unknown); returning false stops it.
HttpResult Download(std::string_view url, const std::filesystem::path& file, const HttpOptions& options,
  const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress = {}) noexcept;

}// namespace OpenDesktopVR::Update
