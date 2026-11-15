// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The product's identity in one place: names, the layer name, kernel object
// names and data folders. Nothing else in the code base spells these out.
#pragma once

#include <filesystem>
#include <string_view>

namespace OpenDesktopVR::Product {

// ADR-017
inline constexpr std::wstring_view DisplayName = L"Open Desktop VR";
inline constexpr std::wstring_view FolderName = L"OpenDesktopVR";
// The one-line theme every user-facing surface follows
inline constexpr std::wstring_view Tagline = L"Use your hands";

// The values below come from cmake/OdvrProduct.cmake, the one place they're
// defined (the layer manifest is generated from the same values).

// ADR-019. Must never change once anyone has installed a build.
inline constexpr std::string_view LayerName = ODVR_LAYER_NAME;
// Setting this environment variable to any value disables the layer (the
// loader honors it through the manifest; the layer checks it too)
inline constexpr std::string_view LayerDisableEnvVar = ODVR_LAYER_DISABLE_ENV;
// "1" shows the built-in test panel; "0" hides it (overrides settings)
inline constexpr std::string_view TestPanelEnvVar = "OPENDESKTOPVR_TEST_PANEL";
// The layer DLL and manifest file names
inline constexpr std::wstring_view LayerDllName = L"" ODVR_LAYER_FILE_STEM ".dll";
inline constexpr std::wstring_view LayerManifestName = L"" ODVR_LAYER_FILE_STEM ".json";

inline constexpr std::string_view Version = ODVR_VERSION;

// Kernel object names live in the session-local namespace (doc 02,
// "Shared state"): Local\OpenDesktopVR.v<abi>.<object>
std::wstring KernelObjectName(std::wstring_view object, unsigned abiVersion);

// Per-user data folders (ADR-009). Empty if the system can't say where
// LocalAppData is. Never throws; never creates anything.
std::filesystem::path DataFolder();
std::filesystem::path LogsFolder();
std::filesystem::path SettingsFile();
std::filesystem::path ProfilesFolder();

}// namespace OpenDesktopVR::Product
