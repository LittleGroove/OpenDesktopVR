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

}// namespace OpenDesktopVR::ProcessUtil
