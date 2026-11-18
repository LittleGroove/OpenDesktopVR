// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <algorithm>
#include <cwctype>
#include <regex>

namespace OpenDesktopVR::WindowMatch {

namespace {

std::wstring Lower(std::wstring_view text) {
  std::wstring lower {text};
  std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return lower;
}

bool TitleMatches(const Rule& rule, std::wstring_view title) {
  if (rule.mTitle.empty()) {
    return true;
  }
  switch (rule.mTitleMode) {
    case TitleMode::Prefer:
      return true;// a preference, not a requirement
    case TitleMode::Exact:
      return StringUtil::EqualsIgnoreCase(rule.mTitle, title);
    case TitleMode::Prefix:
      return Lower(title).starts_with(Lower(rule.mTitle));
    case TitleMode::Contains:
      return Lower(title).find(Lower(rule.mTitle)) != std::wstring::npos;
    case TitleMode::Regex:
      try {
        return std::regex_search(std::wstring {title}, std::wregex {rule.mTitle, std::regex::icase});
      } catch (...) {
        return false;// a malformed pattern never matches
      }
  }
  return false;
}

}// namespace

std::string_view TitleModeName(TitleMode mode) noexcept {
  switch (mode) {
    case TitleMode::Exact:
      return "exact";
    case TitleMode::Prefix:
      return "prefix";
    case TitleMode::Contains:
      return "contains";
    case TitleMode::Regex:
      return "regex";
    case TitleMode::Prefer:
      return "prefer";
  }
  return "prefer";
}

std::optional<TitleMode> ParseTitleMode(std::string_view name) noexcept {
  for (const auto mode: {TitleMode::Exact, TitleMode::Prefix, TitleMode::Contains, TitleMode::Regex, TitleMode::Prefer}) {
    if (name == TitleModeName(mode)) {
      return mode;
    }
  }
  return std::nullopt;
}

Rule FromWindow(const WindowUtil::WindowInfo& window) {
  return {window.mExecutablePath, window.mClassName, window.mTitle, TitleMode::Prefer};
}

bool Matches(const Rule& rule, const WindowUtil::WindowInfo& window) noexcept {
  try {
    if (!rule.mExecutablePath.empty() && !StringUtil::EqualsIgnoreCase(rule.mExecutablePath, window.mExecutablePath)) {
      return false;
    }
    if (!rule.mClassName.empty() && !StringUtil::EqualsIgnoreCase(rule.mClassName, window.mClassName)) {
      return false;
    }
    return TitleMatches(rule, window.mTitle);
  } catch (...) {
    return false;
  }
}

std::optional<WindowUtil::WindowInfo> FindWindow(const Rule& rule) noexcept {
  try {
    std::optional<WindowUtil::WindowInfo> best;
    int bestScore = -1;
    const auto wanted = Lower(rule.mTitle);
    for (auto& window: WindowUtil::ListCaptureCandidates()) {
      if (!Matches(rule, window)) {
        continue;
      }
      int score = 0;
      if (!wanted.empty()) {
        const auto title = Lower(window.mTitle);
        score = title == wanted ? 3 : title.starts_with(wanted) ? 2 : title.find(wanted) != std::wstring::npos ? 1 : 0;
      }
      if (score > bestScore) {
        bestScore = score;
        best = std::move(window);
      }
    }
    return best;
  } catch (...) {
    return std::nullopt;
  }
}

JsonUtil::Json ToJson(const Rule& rule) {
  return {
    {"executable", StringUtil::Narrow(rule.mExecutablePath)},
    {"class", StringUtil::Narrow(rule.mClassName)},
    {"title", StringUtil::Narrow(rule.mTitle)},
    {"titleMode", std::string {TitleModeName(rule.mTitleMode)}},
  };
}

std::optional<Rule> FromJson(const JsonUtil::Json& json) noexcept {
  try {
    if (!json.is_object()) {
      return std::nullopt;
    }
    Rule rule;
    rule.mExecutablePath = StringUtil::Widen(JsonUtil::GetOr<std::string>(json, "executable", ""));
    rule.mClassName = StringUtil::Widen(JsonUtil::GetOr<std::string>(json, "class", ""));
    rule.mTitle = StringUtil::Widen(JsonUtil::GetOr<std::string>(json, "title", ""));
    rule.mTitleMode = ParseTitleMode(JsonUtil::GetOr<std::string>(json, "titleMode", "prefer")).value_or(TitleMode::Prefer);
    if (rule.mExecutablePath.empty() && rule.mClassName.empty() && rule.mTitle.empty()) {
      return std::nullopt;// would match every window
    }
    return rule;
  } catch (...) {
    return std::nullopt;
  }
}

}// namespace OpenDesktopVR::WindowMatch
