// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// String helpers: UTF-8 / UTF-16 conversion, safe truncation, comparisons.
// None of them throw; invalid input is replaced, never rejected (TH-9).
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace OpenDesktopVR::StringUtil {

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view utf16);

// The longest prefix of `utf8` that fits in `maxBytes` without splitting a
// multi-byte character.
std::string_view TruncateUtf8(std::string_view utf8, std::size_t maxBytes) noexcept;

// `utf8` without a trailing, incomplete multi-byte character (what a
// byte-limited formatter leaves when it cuts mid-character)
std::string_view TrimIncompleteUtf8Tail(std::string_view utf8) noexcept;

// Copies as much of `utf8` as fits into `out` (always zero-terminated, never
// splitting a character). Returns the number of bytes copied, excluding the
// terminator. No allocation.
std::size_t CopyTruncated(std::string_view utf8, std::span<char> out) noexcept;

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept;
// `part` appears in `text`, ignoring case (ordinal, like EqualsIgnoreCase).
// An empty `part` is found everywhere.
bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part) noexcept;

// The file name part of a path, for example the game executable's name
std::wstring_view FileNamePart(std::wstring_view path) noexcept;

}// namespace OpenDesktopVR::StringUtil
