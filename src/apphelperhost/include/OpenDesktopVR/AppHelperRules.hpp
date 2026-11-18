// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Which captured windows need the app helper to keep painting while covered
// (CT-3, doc 06 "Occlusion", ADR-031). Measured on Windows 11 (M3d): Edge
// and Firefox both stop painting about a second after another app's window
// covers theirs, and start again only when uncovered.
#pragma once

#include <string_view>

namespace OpenDesktopVR::AppHelper {

enum class AppFamily {
  None,
  // Chromium-based: Edge, Chrome, and Electron or CEF apps such as Discord
  // and Spotify. Top-level class "Chrome_WidgetWin_<n>"
  Chromium,
  // Firefox (its occlusion check is a port of Chromium's). Top-level class
  // "MozillaWindowClass"
  Firefox,
};

// From the captured top-level window's class name
AppFamily FamilyOf(std::wstring_view windowClass) noexcept;

// "Chromium-based", "Firefox", "other"
std::string_view FamilyName(AppFamily family) noexcept;

}// namespace OpenDesktopVR::AppHelper
