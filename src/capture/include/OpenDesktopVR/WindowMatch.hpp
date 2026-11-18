// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Finding a window again after its app restarts (doc 04): a match rule, never
// an HWND (ST-8). The rule is the executable path, the window class and a
// title pattern. Empty parts match anything. Comparisons ignore case.
//
// Titles change (browser tabs, the song in Spotify, the channel in Discord),
// so new rules use TitleMode::Prefer: the executable and class must match,
// and the saved title only chooses between several matching windows (test
// round 4).
#pragma once

#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace OpenDesktopVR::WindowMatch {

enum class TitleMode { Exact, Prefix, Contains, Regex, Prefer };

std::string_view TitleModeName(TitleMode mode) noexcept;
std::optional<TitleMode> ParseTitleMode(std::string_view name) noexcept;

struct Rule {
  std::wstring mExecutablePath;
  std::wstring mClassName;
  std::wstring mTitle;
  TitleMode mTitleMode {TitleMode::Prefer};

  bool operator==(const Rule&) const = default;
};

// A rule for this window: its executable and class, with its current title
// as the preference among several matching windows
Rule FromWindow(const WindowUtil::WindowInfo& window);

// A malformed regex never matches (and never throws)
bool Matches(const Rule& rule, const WindowUtil::WindowInfo& window) noexcept;

// The best match among the current candidates: with a title preference,
// the window whose title equals it, else starts with it, else contains it,
// else the first match
std::optional<WindowUtil::WindowInfo> FindWindow(const Rule& rule) noexcept;

JsonUtil::Json ToJson(const Rule& rule);
// Untrusted input (TH-9): nullopt for anything malformed
std::optional<Rule> FromJson(const JsonUtil::Json& json) noexcept;

}// namespace OpenDesktopVR::WindowMatch
