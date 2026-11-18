// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Redaction.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <ShlObj.h>

#include <algorithm>
#include <cctype>

namespace OpenDesktopVR::Diagnostics {

namespace {

char Lower(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool MatchesAt(std::string_view text, std::size_t at, std::string_view word) noexcept {
  if (at + word.size() > text.size()) {
    return false;
  }
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (Lower(text[at + i]) != Lower(word[i])) {
      return false;
    }
  }
  return true;
}

std::string ReplaceAll(std::string_view text, std::string_view from, std::string_view to) {
  if (from.empty()) {
    return std::string {text};
  }
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (MatchesAt(text, i, from)) {
      out += to;
      i += from.size();
    } else {
      out += text[i++];
    }
  }
  return out;
}

bool IsSeparator(char c) noexcept {
  return c == '\\' || c == '/';
}

// "<sep>Users<sep>" then a name: the name becomes <user>
std::string RedactUserFolders(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (IsSeparator(text[i]) && MatchesAt(text, i + 1, "users")) {
      // the separators after "Users"
      std::size_t j = i + 1 + 5;
      std::size_t separators = 0;
      while (j < text.size() && IsSeparator(text[j])) {
        ++j;
        ++separators;
      }
      if (separators > 0) {
        std::size_t end = j;
        while (end < text.size() && !IsSeparator(text[end]) && text[end] != '"' && text[end] != '\'' && text[end] != '\r'
               && text[end] != '\n' && text[end] != ';' && text[end] != ',' && text[end] != ')') {
          ++end;
        }
        const auto name = text.substr(j, end - j);
        out.append(text.substr(i, j - i));
        const bool keep = name.empty() || MatchesAt(name, 0, "public") || MatchesAt(name, 0, "default")
          || MatchesAt(name, 0, "all users") || name.front() == '<' || name.front() == '%';
        out.append(keep ? std::string {name} : std::string {"<user>"});
        i = end;
        continue;
      }
    }
    out += text[i++];
  }
  return out;
}

std::string Doubled(std::string_view path) {
  std::string out;
  for (const char c: path) {
    out += c;
    if (c == '\\') {
      out += '\\';
    }
  }
  return out;
}

}// namespace

Redaction Redaction::ForProfile(std::string_view profileFolder) {
  Redaction redaction;
  std::string folder {profileFolder};
  while (!folder.empty() && IsSeparator(folder.back())) {
    folder.pop_back();
  }
  if (folder.size() < 4) {
    return redaction;// not a real profile path: nothing to do
  }
  std::string forward = folder;
  std::ranges::replace(forward, '\\', '/');
  // The longest spelling first, so a doubled one isn't half-replaced
  redaction.mFolders.emplace_back(Doubled(folder), "%USERPROFILE%");
  redaction.mFolders.emplace_back(folder, "%USERPROFILE%");
  redaction.mFolders.emplace_back(forward, "%USERPROFILE%");
  return redaction;
}

Redaction Redaction::ForThisUser() {
  PWSTR path = nullptr;
  std::string profile;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path)) && path) {
    profile = StringUtil::Narrow(path);
  }
  CoTaskMemFree(path);
  return ForProfile(profile);
}

std::string Redact(std::string_view text, const Redaction& redaction) {
  std::string out {text};
  for (const auto& [from, to]: redaction.mFolders) {
    out = ReplaceAll(out, from, to);
  }
  return RedactUserFolders(out);
}

}// namespace OpenDesktopVR::Diagnostics
