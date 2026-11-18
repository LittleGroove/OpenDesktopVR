// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Layer registration (doc 03 "Registration"; CMP-2, CMP-3), against a
// private registry key so the real loader settings are never touched.

#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/Product.hpp>

#include <Windows.h>

#include <filesystem>
#include <format>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Registration;
using namespace OpenDesktopVR::Testing;
namespace fs = std::filesystem;

namespace {

void WriteManifest(const fs::path& path, std::string_view layerName) {
  FileUtil::WriteAtomic(
    path, std::format(R"({{"file_format_version": "1.0.0", "api_layer": {{"name": "{}"}}}})", layerName));
}

DWORD ValueOf(const RegistryLocation& location, const fs::path& name) {
  DWORD data = 0xFFFF;
  DWORD size = sizeof(data);
  RegGetValueW(location.mRoot, location.mKeyPath.c_str(), name.c_str(), RRF_RT_REG_DWORD, nullptr, &data, &size);
  return data;
}

}// namespace

int main() {
  TempFolder folder;
  RegistryLocation location;
  const auto testRoot = std::format(L"Software\\{}-Test-{}", Product::FolderName, GetCurrentProcessId());
  location.mKeyPath = testRoot + L"\\Implicit";

  const fs::path root {folder.Path()};
  const auto current = root / L"new" / Product::LayerManifestName;
  const auto older = root / L"old" / Product::LayerManifestName;
  const auto missing = root / L"gone" / Product::LayerManifestName;
  const auto other = root / L"other" / L"SomeoneElse.json";
  WriteManifest(current, Product::LayerName);
  WriteManifest(older, Product::LayerName);
  WriteManifest(other, "XR_APILAYER_SOMEONE_else");

  Scenario("Registering enables ours and disables our older entries (CMP-3)", [&] {
    {
      HKEY key = nullptr;
      RegCreateKeyExW(location.mRoot, location.mKeyPath.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
      const DWORD zero = 0;
      for (const auto& path: {older, missing, other}) {
        RegSetValueExW(key, path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
      }
      RegCloseKey(key);
    }
    const auto result = Register(current, location);
    Check(result.mOk, "registered");
    Check(ValueOf(location, current) == 0, "our manifest is enabled (0)");
    Check(ValueOf(location, older) == 1, "our older install is disabled (1), not deleted");
    Check(ValueOf(location, missing) == 1, "our dangling entry (manifest gone) is disabled too");
    Check(ValueOf(location, other) == 0, "another product's entry is untouched (CMP-2)");
    Check(result.mStaleDisabled == 2, "two stale entries reported");
  });

  Scenario("Listing reads names and spots missing manifests", [&] {
    const auto entries = ListLayers(location);
    Check(entries.size() == 4, std::format("4 entries ({})", entries.size()));
    int ours = 0;
    bool sawMissing = false;
    for (const auto& entry: entries) {
      ours += entry.mIsOurs ? 1 : 0;
      sawMissing = sawMissing || (!entry.mManifestExists && entry.mManifestPath == missing);
    }
    Check(ours == 3, "three of them are ours");
    Check(sawMissing, "the missing manifest is flagged");
  });

  Scenario("Registering a manifest that doesn't exist fails cleanly", [&] {
    const auto result = Register(root / L"nowhere.json", location);
    Check(!result.mOk && result.mError == ERROR_FILE_NOT_FOUND, "refused with 'file not found'");
  });

  Scenario("Unregistering removes only ours", [&] {
    Check(Unregister(location) == 3, "three of ours removed");
    const auto entries = ListLayers(location);
    Check(entries.size() == 1 && !entries[0].mIsOurs, "the other product's entry remains");
  });

  Scenario("A per-user entry is listed once, though both registry views show it (test round 1)", [&] {
    const auto entry = [](HKEY root, const wchar_t* where, const wchar_t* path) {
      LayerEntry e;
      e.mRoot = root;
      e.mWhere = where;
      e.mManifestPath = path;
      return e;
    };
    const auto merged = MergeSharedViews({
      entry(HKEY_CURRENT_USER, L"HKCU (64-bit)", L"C:\\Users\\A\\Layer.json"),
      entry(HKEY_CURRENT_USER, L"HKCU (32-bit)", L"c:\\users\\a\\layer.json"),
      entry(HKEY_LOCAL_MACHINE, L"HKLM (64-bit)", L"C:\\Other\\Other.json"),
      entry(HKEY_LOCAL_MACHINE, L"HKLM (32-bit)", L"C:\\Other\\Other.json"),
    });
    Check(merged.size() == 3, std::format("3 entries after merging ({})", merged.size()));
    Check(merged.size() == 3 && merged[0].mWhere == L"HKCU", "the per-user entry is labelled HKCU, without a view");
    Check(
      merged.size() == 3 && merged[1].mWhere == L"HKLM (64-bit)" && merged[2].mWhere == L"HKLM (32-bit)",
      "machine-wide views are separate keys: both kept");
  });

  RegDeleteTreeW(HKEY_CURRENT_USER, testRoot.c_str());
  return Finish();
}
