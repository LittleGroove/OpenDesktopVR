// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// "Start with Windows" (UI-7): a value in the per-user Run key pointing at
// the host. Per user, so it needs no elevation. Tests pass their own key.
#pragma once

#include <filesystem>

namespace OpenDesktopVR::Autostart {

inline constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t ValueName[] = L"Open Desktop VR";

// The value is there and starts this executable
bool IsEnabled(const std::filesystem::path& executable, const wchar_t* key = RunKey) noexcept;
// Adds or removes it; false if the registry refused
bool SetEnabled(bool enabled, const std::filesystem::path& executable, const wchar_t* key = RunKey) noexcept;

}// namespace OpenDesktopVR::Autostart
