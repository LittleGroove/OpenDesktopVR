// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

namespace OpenDesktopVR::ProcessUtil {

std::optional<std::string> GetEnvironment(std::string_view name) noexcept {
  try {
    const auto wideName = StringUtil::Widen(name);
    const DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (size == 0) {
      return std::nullopt;// not set (or empty, which Windows treats the same)
    }
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
    if (length == 0 || length >= size) {
      return std::nullopt;
    }
    value.resize(length);
    return StringUtil::Narrow(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::filesystem::path ExecutablePath() noexcept {
  try {
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
      if (length == 0) {
        return {};
      }
      if (length < path.size()) {
        path.resize(length);
        return path;
      }
      path.resize(path.size() * 2);
    }
  } catch (...) {
  }
  return {};
}

std::wstring ExecutableStem() noexcept {
  try {
    return ExecutablePath().stem().wstring();
  } catch (...) {
    return {};
  }
}

}// namespace OpenDesktopVR::ProcessUtil
