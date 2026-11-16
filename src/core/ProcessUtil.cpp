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

namespace OpenDesktopVR::ProcessUtil {

namespace {

Elevation TokenElevationOf(HANDLE process) noexcept {
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
    return Elevation::Unknown;
  }
  TOKEN_ELEVATION elevation {};
  DWORD size = 0;
  const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  if (!ok) {
    return Elevation::Unknown;
  }
  return elevation.TokenIsElevated ? Elevation::Elevated : Elevation::NotElevated;
}

}// namespace

std::string_view ElevationName(Elevation elevation) noexcept {
  switch (elevation) {
    case Elevation::Elevated:
      return "elevated";
    case Elevation::NotElevated:
      return "not elevated";
    case Elevation::Unknown:
      return "unknown (probably elevated)";
  }
  return "unknown";
}

Elevation CurrentProcessElevation() noexcept {
  return TokenElevationOf(GetCurrentProcess());
}

Elevation ProcessElevation(unsigned long processID) noexcept {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processID);
  if (!process) {
    return Elevation::Unknown;
  }
  const auto elevation = TokenElevationOf(process);
  CloseHandle(process);
  return elevation;
}

std::string_view CurrentElevationType() noexcept {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return "unknown";
  }
  TOKEN_ELEVATION_TYPE type {};
  DWORD size = 0;
  const bool ok = GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);
  CloseHandle(token);
  if (!ok) {
    return "unknown";
  }
  switch (type) {
    case TokenElevationTypeFull:
      return "full (elevated)";
    case TokenElevationTypeLimited:
      return "limited (an administrator, filtered by UAC)";
    case TokenElevationTypeDefault:
      return "default (a standard user, or UAC off)";
  }
  return "unknown";
}

std::optional<bool> UacEnabled() noexcept {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System)", L"EnableLUA",
        RRF_RT_REG_DWORD, nullptr, &value, &size)
      != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value != 0;
}

std::filesystem::path ProcessImagePath(unsigned long processID) noexcept {
  try {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processID);
    if (!process) {
      return {};
    }
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = static_cast<DWORD>(std::size(path));
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    return ok ? std::filesystem::path {std::wstring_view {path, size}} : std::filesystem::path {};
  } catch (...) {
    return {};
  }
}

std::optional<bool> SameArchitecture(unsigned long processID) noexcept {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processID);
  if (!process) {
    return std::nullopt;
  }
  USHORT theirs = IMAGE_FILE_MACHINE_UNKNOWN;
  USHORT native = IMAGE_FILE_MACHINE_UNKNOWN;
  const bool ok = IsWow64Process2(process, &theirs, &native);
  CloseHandle(process);
  if (!ok) {
    return std::nullopt;
  }
  // UNKNOWN: not under WOW64, so the PC's own architecture; ours is x64 too
  // unless we run under WOW64 ourselves, which a 64-bit build never does
  return theirs == IMAGE_FILE_MACHINE_UNKNOWN && (native == IMAGE_FILE_MACHINE_AMD64 || sizeof(void*) == 4);
}

}// namespace OpenDesktopVR::ProcessUtil
