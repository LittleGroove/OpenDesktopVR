// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Paths in the debug bundle's text lose the user's name (doc 09 "Debug
// bundle": "settings with paths redacted"):
// - this user's profile folder becomes %USERPROFILE%, however it's written
//   (backslashes, JSON's doubled ones, forward slashes; any case);
// - any other "<drive>:\Users\<name>" becomes "<drive>:\Users\<user>"
//   (Public and Default stay).
//
// Only paths: a bare word equal to the user's name is left alone, so
// ordinary words in logs survive. The privacy notice says what remains
// (window titles, crash dumps).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Diagnostics {

struct Redaction {
  // Replaced first, whole and case-insensitively: (from, to)
  std::vector<std::pair<std::string, std::string>> mFolders;

  // This user's profile folder
  static Redaction ForThisUser();
  // For tests: a given profile folder ("C:\Users\Someone")
  static Redaction ForProfile(std::string_view profileFolder);
};

std::string Redact(std::string_view text, const Redaction& redaction);

}// namespace OpenDesktopVR::Diagnostics
