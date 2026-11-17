// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Product.hpp>

#include <Windows.h>

#include <ShlObj.h>

#include <format>
#include <fstream>
#include <string>

namespace OpenDesktopVR::Product {

std::wstring KernelObjectName(std::wstring_view object, unsigned abiVersion) {
  return std::format(L"Local\\{}.v{}.{}", FolderName, abiVersion, object);
}

std::filesystem::path DataFolder() {
  // Tests: a private folder (OPENDESKTOPVR_DATA_FOLDER), so a test never
  // touches the user's settings
  wchar_t overridden[MAX_PATH] {};
  if (const DWORD length = GetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", overridden, MAX_PATH);
      length > 0 && length < MAX_PATH) {
    return std::filesystem::path {overridden};
  }
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
    result = std::filesystem::path {raw} / FolderName;
  }
  CoTaskMemFree(raw);
  return result;
}

std::filesystem::path LogsFolder() {
  const auto root = DataFolder();
  return root.empty() ? root : root / L"logs";
}

std::filesystem::path CrashDumpsFolder() {
  const auto root = DataFolder();
  return root.empty() ? root : root / L"crashdumps";
}

std::filesystem::path SettingsFile() {
  const auto root = DataFolder();
  return root.empty() ? root : root / L"settings.json";
}

std::filesystem::path ProfilesFolder() {
  const auto root = DataFolder();
  return root.empty() ? root : root / L"profiles";
}

std::string BuildDescription() {
  try {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&BuildDescription), &self)) {
      return "unknown (the module couldn't be found)";
    }
    wchar_t path[MAX_PATH * 2] {};
    const DWORD length = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
      return "unknown (the module's path couldn't be read)";
    }
    const auto folder = std::filesystem::path {std::wstring_view {path, length}}.parent_path();
    for (const auto& candidate: {folder / L"BUILD-INFO.txt", folder.parent_path() / L"BUILD-INFO.txt"}) {
      std::ifstream file {candidate};
      if (!file) {
        continue;
      }
      std::string commit;
      std::string built;
      const auto value = [](const std::string& line, std::string_view key) {
        auto text = line.substr(key.size());
        const auto first = text.find_first_not_of(" \t");
        const auto last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string {} : text.substr(first, last - first + 1);
      };
      for (std::string line; std::getline(file, line);) {
        if (line.starts_with("Commit:")) {
          commit = value(line, "Commit:");
        } else if (line.starts_with("Built:")) {
          built = value(line, "Built:");
        }
      }
      return std::format("commit {}, built {}", commit.empty() ? "unknown" : commit, built.empty() ? "unknown" : built);
    }
    return "a development build (no BUILD-INFO.txt)";
  } catch (...) {
    return "unknown";
  }
}

}// namespace OpenDesktopVR::Product
