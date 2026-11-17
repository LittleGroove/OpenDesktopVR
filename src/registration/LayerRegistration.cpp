// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>

namespace OpenDesktopVR::Registration {

namespace {

class Key {
 public:
  Key() = default;
  ~Key() {
    if (mKey) {
      RegCloseKey(mKey);
    }
  }
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  HKEY* put() {
    return &mKey;
  }
  HKEY get() const {
    return mKey;
  }

 private:
  HKEY mKey {nullptr};
};

struct RawValue {
  std::wstring mName;
  DWORD mData {0};
  bool mIsDword {false};
};

std::vector<RawValue> ReadValues(HKEY key) {
  std::vector<RawValue> values;
  std::wstring name(32767, L'\0');// the longest registry value name
  for (DWORD index = 0;; ++index) {
    DWORD nameLength = static_cast<DWORD>(name.size());
    DWORD type = 0;
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    const auto status = RegEnumValueW(
      key, index, name.data(), &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize);
    if (status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) {
      continue;
    }
    values.push_back({name.substr(0, nameLength), data, type == REG_DWORD && status == ERROR_SUCCESS});
  }
  return values;
}

}// namespace

std::wstring RegistryLocation::Describe() const {
  const std::wstring root = mRoot == HKEY_LOCAL_MACHINE ? L"HKLM" : (mRoot == HKEY_CURRENT_USER ? L"HKCU" : L"HK?");
  return root + ((mView & KEY_WOW64_32KEY) ? L" (32-bit)" : L" (64-bit)");
}

RegistryLocation UserImplicitLayers() {
  return {};
}

ManifestInfo ReadManifest(const std::filesystem::path& manifest) noexcept {
  ManifestInfo info;
  const auto text = FileUtil::ReadAll(manifest);
  if (!text) {
    return info;
  }
  const auto json = JsonUtil::Parse(*text);
  if (!json) {
    return info;
  }
  const auto layer = JsonUtil::Get<JsonUtil::Json>(*json, "api_layer");
  if (!layer) {
    return info;
  }
  info.mLayerName = JsonUtil::Get<std::string>(*layer, "name");
  info.mLibraryPath = JsonUtil::Get<std::string>(*layer, "library_path");
  return info;
}

std::optional<std::string> ReadManifestLayerName(const std::filesystem::path& manifest) noexcept {
  return ReadManifest(manifest).mLayerName;
}

bool IsOurs(const std::filesystem::path& manifest, const std::optional<std::string>& layerName) noexcept {
  if (layerName) {
    return *layerName == Product::LayerName;
  }
  return StringUtil::EqualsIgnoreCase(manifest.filename().wstring(), Product::LayerManifestName);
}

std::vector<LayerEntry> ListLayers(const RegistryLocation& location) {
  std::vector<LayerEntry> entries;
  Key key;
  if (RegOpenKeyExW(location.mRoot, location.mKeyPath.c_str(), 0, KEY_READ | location.mView, key.put()) != ERROR_SUCCESS) {
    return entries;
  }
  for (const auto& value: ReadValues(key.get())) {
    LayerEntry entry;
    entry.mRoot = location.mRoot;
    entry.mWhere = location.Describe();
    entry.mManifestPath = value.mName;
    entry.mEnabled = value.mIsDword && value.mData == 0;
    entry.mManifestExists = FileUtil::Exists(entry.mManifestPath);
    auto manifest = ReadManifest(entry.mManifestPath);
    entry.mLayerName = std::move(manifest.mLayerName);
    entry.mLibraryPath = std::move(manifest.mLibraryPath);
    entry.mIsOurs = IsOurs(entry.mManifestPath, entry.mLayerName);
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::vector<LayerEntry> ListAllImplicitLayers() {
  std::vector<LayerEntry> all;
  for (const auto root: {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    for (const auto view: {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
      RegistryLocation location;
      location.mRoot = root;
      location.mView = static_cast<REGSAM>(view);
      auto entries = ListLayers(location);
      all.insert(all.end(), entries.begin(), entries.end());
    }
  }
  return MergeSharedViews(std::move(all));
}

std::vector<LayerEntry> MergeSharedViews(std::vector<LayerEntry> entries) {
  std::vector<LayerEntry> merged;
  for (auto& entry: entries) {
    // Only HKCU is shared; HKLM's two views are separate keys, so an entry
    // in both is two real registrations
    const bool seen = entry.mRoot == HKEY_CURRENT_USER && std::ranges::any_of(merged, [&](const LayerEntry& kept) {
      return kept.mRoot == entry.mRoot
        && StringUtil::EqualsIgnoreCase(kept.mManifestPath.wstring(), entry.mManifestPath.wstring());
    });
    if (seen) {
      continue;
    }
    // HKCU\Software is one key in both views, so its view label means nothing
    if (entry.mRoot == HKEY_CURRENT_USER) {
      entry.mWhere = L"HKCU";
    }
    merged.push_back(std::move(entry));
  }
  return merged;
}

RegisterResult Register(const std::filesystem::path& manifest, const RegistryLocation& location) {
  RegisterResult result;
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(manifest, ec);
  if (ec || !FileUtil::Exists(absolute)) {
    result.mError = ERROR_FILE_NOT_FOUND;
    return result;
  }
  Key key;
  result.mError = RegCreateKeyExW(
    location.mRoot,
    location.mKeyPath.c_str(),
    0,
    nullptr,
    0,
    KEY_READ | KEY_SET_VALUE | location.mView,
    nullptr,
    key.put(),
    nullptr);
  if (result.mError != ERROR_SUCCESS) {
    return result;
  }
  const auto ourPath = absolute.wstring();
  for (const auto& value: ReadValues(key.get())) {
    if (StringUtil::EqualsIgnoreCase(value.mName, ourPath)) {
      continue;
    }
    const std::filesystem::path other {value.mName};
    if (!IsOurs(other, ReadManifestLayerName(other))) {
      continue;// never touch other products' entries
    }
    if (value.mIsDword && value.mData != 0) {
      continue;// already disabled
    }
    const DWORD disabled = 1;
    if (
      RegSetValueExW(key.get(), value.mName.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&disabled), sizeof(disabled))
      == ERROR_SUCCESS) {
      ++result.mStaleDisabled;
      Log::Info("Registration: disabled our older entry {}", StringUtil::Narrow(value.mName));
    }
  }
  const DWORD enabled = 0;
  result.mError = RegSetValueExW(
    key.get(), ourPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
  result.mOk = result.mError == ERROR_SUCCESS;
  return result;
}

int Unregister(const RegistryLocation& location) {
  Key key;
  if (
    RegOpenKeyExW(location.mRoot, location.mKeyPath.c_str(), 0, KEY_READ | KEY_SET_VALUE | location.mView, key.put())
    != ERROR_SUCCESS) {
    return 0;
  }
  int removed = 0;
  for (const auto& value: ReadValues(key.get())) {
    const std::filesystem::path path {value.mName};
    if (IsOurs(path, ReadManifestLayerName(path)) && RegDeleteValueW(key.get(), value.mName.c_str()) == ERROR_SUCCESS) {
      ++removed;
    }
  }
  return removed;
}

}// namespace OpenDesktopVR::Registration
