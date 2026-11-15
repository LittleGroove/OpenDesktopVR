// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Finding a window again after its app restarts (doc 04): a match rule, never
// an HWND (ST-8). The rule is the executable path, the window class and a
// title pattern. Empty parts match anything. Comparisons ignore case.
#pragma once

#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace OpenDesktopVR::WindowMatch {

enum class TitleMode { Exact, Prefix, Contains, Regex };

std::string_view TitleModeName(TitleMode mode) noexcept;
std::optional<TitleMode> ParseTitleMode(std::string_view name) noexcept;

struct Rule {
  std::wstring mExecutablePath;
  std::wstring mClassName;
  std::wstring mTitle;
  TitleMode mTitleMode {TitleMode::Contains};

  bool operator==(const Rule&) const = default;
};

// A rule for this window: its executable, its class, and its title as
// "contains" (titles often carry changing parts, like an unread count)
Rule FromWindow(const WindowUtil::WindowInfo& window);

// A malformed regex never matches (and never throws)
bool Matches(const Rule& rule, const WindowUtil::WindowInfo& window) noexcept;

// The best match among the current candidates, if any
std::optional<WindowUtil::WindowInfo> FindWindow(const Rule& rule) noexcept;

JsonUtil::Json ToJson(const Rule& rule);
// Untrusted input (TH-9): nullopt for anything malformed
std::optional<Rule> FromJson(const JsonUtil::Json& json) noexcept;

}// namespace OpenDesktopVR::WindowMatch
