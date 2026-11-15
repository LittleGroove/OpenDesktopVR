// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Registering the layer with the OpenXR loader (doc 03 "Registration"):
// - per user, under HKCU, no admin (ADR-009): the value name is the manifest
//   path, the DWORD data 0 means enabled;
// - our own other entries (an older install path) are disabled by setting
//   them to 1, never deleted, so two copies never load (CMP-3);
// - other products' entries are never touched (CMP-2).
#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::Registration {

struct RegistryLocation {
  HKEY mRoot {HKEY_CURRENT_USER};
  std::wstring mKeyPath {L"Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit"};
  REGSAM mView {KEY_WOW64_64KEY};
  std::wstring Describe() const;
};

// Where the product registers itself
RegistryLocation UserImplicitLayers();

struct LayerEntry {
  std::wstring mWhere;// for example "HKCU (64-bit)"
  std::filesystem::path mManifestPath;
  bool mEnabled {false};// data is DWORD 0
  bool mManifestExists {false};
  std::optional<std::string> mLayerName;// from the manifest, if readable
  bool mIsOurs {false};
};

// The layer name in a manifest file, if it can be read
std::optional<std::string> ReadManifestLayerName(const std::filesystem::path& manifest) noexcept;

// Ours if the manifest names our layer, or (when it's gone) has our file name
bool IsOurs(const std::filesystem::path& manifest, const std::optional<std::string>& layerName) noexcept;

std::vector<LayerEntry> ListLayers(const RegistryLocation& location);

// Every implicit layer in both hives and both registry views, for
// diagnostics and the debug bundle (LG-4)
std::vector<LayerEntry> ListAllImplicitLayers();

struct RegisterResult {
  bool mOk {false};
  int mStaleDisabled {0};
  long mError {ERROR_SUCCESS};
};

RegisterResult Register(const std::filesystem::path& manifest, const RegistryLocation& location = UserImplicitLayers());

// Removes every entry of ours at `location`; returns how many
int Unregister(const RegistryLocation& location = UserImplicitLayers());

}// namespace OpenDesktopVR::Registration
