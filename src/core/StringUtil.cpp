// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace OpenDesktopVR::StringUtil {

std::wstring Widen(std::string_view utf8) {
  if (utf8.empty() || utf8.size() > INT_MAX) {
    return {};
  }
  const auto inLength = static_cast<int>(utf8.size());
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), inLength, nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), inLength, result.data(), length);
  return result;
}

std::string Narrow(std::wstring_view utf16) {
  if (utf16.empty() || utf16.size() > INT_MAX) {
    return {};
  }
  const auto inLength = static_cast<int>(utf16.size());
  const int length = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), inLength, nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, utf16.data(), inLength, result.data(), length, nullptr, nullptr);
  return result;
}

std::string_view TruncateUtf8(std::string_view utf8, std::size_t maxBytes) noexcept {
  if (utf8.size() <= maxBytes) {
    return utf8;
  }
  std::size_t end = maxBytes;
  // Step back over continuation bytes (10xxxxxx) to a character start
  while (end > 0 && (static_cast<unsigned char>(utf8[end]) & 0xC0) == 0x80) {
    --end;
  }
  return utf8.substr(0, end);
}

std::string_view TrimIncompleteUtf8Tail(std::string_view utf8) noexcept {
  // Look back at most 3 bytes for the start of the last character
  const std::size_t size = utf8.size();
  for (std::size_t back = 1; back <= 4 && back <= size; ++back) {
    const auto byte = static_cast<unsigned char>(utf8[size - back]);
    if ((byte & 0xC0) == 0x80) {
      continue;// continuation byte
    }
    std::size_t expected = 1;
    if ((byte & 0xE0) == 0xC0) {
      expected = 2;
    } else if ((byte & 0xF0) == 0xE0) {
      expected = 3;
    } else if ((byte & 0xF8) == 0xF0) {
      expected = 4;
    }
    return back < expected ? utf8.substr(0, size - back) : utf8;
  }
  return utf8;
}

std::size_t CopyTruncated(std::string_view utf8, std::span<char> out) noexcept {
  if (out.empty()) {
    return 0;
  }
  const auto fitted = TruncateUtf8(utf8, out.size() - 1);
  std::memcpy(out.data(), fitted.data(), fitted.size());
  out[fitted.size()] = '\0';
  return fitted.size();
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
  if (a.size() != b.size() || a.size() > INT_MAX) {
    return false;
  }
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
    == CSTR_EQUAL;
}

bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part) noexcept {
  if (part.empty()) {
    return true;
  }
  if (part.size() > text.size() || text.size() > INT_MAX) {
    return false;
  }
  // Short strings (titles, file names): compare at each position
  for (std::size_t at = 0; at + part.size() <= text.size(); ++at) {
    if (CompareStringOrdinal(text.data() + at, static_cast<int>(part.size()), part.data(), static_cast<int>(part.size()), TRUE)
        == CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

std::wstring_view FileNamePart(std::wstring_view path) noexcept {
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

}// namespace OpenDesktopVR::StringUtil
