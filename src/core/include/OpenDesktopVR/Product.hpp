// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The product's identity in one place: names, the layer name, kernel object
// names and data folders. Nothing else in the code base spells these out.
#pragma once

#include <filesystem>
#include <string>
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
// Loaded into captured apps that need it, next to the host's exe (doc 06)
inline constexpr std::wstring_view AppHelperDllName = L"" ODVR_APP_HELPER_FILE_STEM ".dll";

inline constexpr std::string_view Version = ODVR_VERSION;

// The GitHub repository, "owner/name": the source code (GPL-3.0-only,
// ADR-044) and the releases updates come from (ADR-045)
inline constexpr std::string_view Repository = ODVR_REPOSITORY;
inline constexpr std::string_view LicenseName = "GNU GPL, version 3";

// Kernel object names live in the session-local namespace (doc 02,
// "Shared state"): Local\OpenDesktopVR.v<abi>.<object>
std::wstring KernelObjectName(std::wstring_view object, unsigned abiVersion);

// Per-user data folders (ADR-009). Empty if the system can't say where
// LocalAppData is. Never throws; never creates anything.
// %LOCALAPPDATA%\<product>; or, for tests, OPENDESKTOPVR_DATA_FOLDER
std::filesystem::path DataFolder();
std::filesystem::path LogsFolder();
// Crash dumps and crashes.txt (CrashHandler; TH-10)
std::filesystem::path CrashDumpsFolder();
std::filesystem::path SettingsFile();
std::filesystem::path ProfilesFolder();

// Which build is running, from the BUILD-INFO.txt a package puts beside the
// running module (or one folder up, for the tools): "commit 111d544170 on
// m4, built 2026-11-16 14:39 (Release)", or "a development build (no
// BUILD-INFO.txt)". So a test round's logs say which build made them. Reads
// a small file: call it once at start, never per frame. Never throws.
std::string BuildDescription();

}// namespace OpenDesktopVR::Product
