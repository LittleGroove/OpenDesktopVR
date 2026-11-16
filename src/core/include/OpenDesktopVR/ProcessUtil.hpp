// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Process helpers: the process environment and executable paths. They read
// the OS's view (not a C runtime's private copy), so a DLL sees the same
// values the OpenXR loader does. None of them throw.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace OpenDesktopVR::ProcessUtil {

// The variable's value, or nullopt if it isn't set
std::optional<std::string> GetEnvironment(std::string_view name) noexcept;

// This process's executable, for example C:\Games\Elite\EliteDangerous64.exe
std::filesystem::path ExecutablePath() noexcept;

// The executable's file name without its extension, for per-game log names
std::wstring ExecutableStem() noexcept;

// ---- Elevation (CMP-8: the top cause of support requests) ----------------

enum class Elevation {
  Elevated,       // running with administrator rights
  NotElevated,    // a standard user, or an administrator with UAC filtering
  Unknown,        // couldn't be read: for another process, usually means
                  // it's elevated and we aren't
};

// "elevated", "not elevated", "unknown (probably elevated)"
std::string_view ElevationName(Elevation elevation) noexcept;

Elevation CurrentProcessElevation() noexcept;
Elevation ProcessElevation(unsigned long processID) noexcept;

// What kind of token this process has: "full" (elevated), "limited" (an
// administrator running filtered by UAC), "default" (a standard user, or UAC
// off), or "unknown"
std::string_view CurrentElevationType() noexcept;

// UAC on (EnableLUA): nullopt if the setting can't be read
std::optional<bool> UacEnabled() noexcept;

// ---- Other processes -------------------------------------------------------

// Its executable's full path; empty if it can't be read
std::filesystem::path ProcessImagePath(unsigned long processID) noexcept;

// Whether a DLL built like this process can load into it: same architecture,
// not a 32-bit process under WOW64 (ADR-015). nullopt if it can't be read.
std::optional<bool> SameArchitecture(unsigned long processID) noexcept;

}// namespace OpenDesktopVR::ProcessUtil
