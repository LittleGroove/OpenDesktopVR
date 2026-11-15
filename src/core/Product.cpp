// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Product.hpp>

#include <Windows.h>

#include <ShlObj.h>

#include <format>

namespace OpenDesktopVR::Product {

std::wstring KernelObjectName(std::wstring_view object, unsigned abiVersion) {
  return std::format(L"Local\\{}.v{}.{}", FolderName, abiVersion, object);
}

std::filesystem::path DataFolder() {
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

std::filesystem::path SettingsFile() {
  const auto root = DataFolder();
  return root.empty() ? root : root / L"settings.json";
}

std::filesystem::path ProfilesFolder() {
  const auto root = DataFolder();
  return root.empty() ? root : root / L"profiles";
}

}// namespace OpenDesktopVR::Product
