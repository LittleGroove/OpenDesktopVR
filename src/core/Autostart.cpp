// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Autostart.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <string>

namespace OpenDesktopVR::Autostart {

namespace {

std::wstring CommandFor(const std::filesystem::path& executable) {
  return L"\"" + executable.wstring() + L"\"";
}

}// namespace

bool IsEnabled(const std::filesystem::path& executable, const wchar_t* key) noexcept {
  try {
    wchar_t value[MAX_PATH * 2] {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, ValueName, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
      return false;
    }
    return StringUtil::EqualsIgnoreCase(value, CommandFor(executable));
  } catch (...) {
    return false;
  }
}

bool SetEnabled(bool enabled, const std::filesystem::path& executable, const wchar_t* key) noexcept {
  try {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) {
      return false;
    }
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
      const auto command = CommandFor(executable);
      status = RegSetValueExW(handle, ValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
      status = RegDeleteValueW(handle, ValueName);
      if (status == ERROR_FILE_NOT_FOUND) {
        status = ERROR_SUCCESS;// already off
      }
    }
    RegCloseKey(handle);
    return status == ERROR_SUCCESS;
  } catch (...) {
    return false;
  }
}

}// namespace OpenDesktopVR::Autostart
