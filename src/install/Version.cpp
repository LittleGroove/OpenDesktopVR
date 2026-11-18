// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Version.hpp>

#include <format>

namespace OpenDesktopVR::Install {

namespace {

// 1 to 5 digits, without a leading zero (except "0" itself)
bool ReadNumber(std::string_view& text, unsigned& value) noexcept {
  std::size_t digits = 0;
  unsigned result = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    if (digits == 5) {
      return false;
    }
    result = result * 10 + static_cast<unsigned>(text[digits] - '0');
    ++digits;
  }
  if (digits == 0 || (digits > 1 && text[0] == '0') || result > MaxVersionPart) {
    return false;
  }
  value = result;
  text.remove_prefix(digits);
  return true;
}

bool Expect(std::string_view& text, std::string_view what) noexcept {
  if (!text.starts_with(what)) {
    return false;
  }
  text.remove_prefix(what.size());
  return true;
}

// A release comes after its betas
unsigned BetaRank(unsigned beta) noexcept {
  return beta == 0 ? ~0u : beta;
}

}// namespace

std::optional<Version> ParseVersion(std::string_view text) noexcept {
  if (text.starts_with('v') || text.starts_with('V')) {
    text.remove_prefix(1);
  }
  Version version;
  if (!ReadNumber(text, version.mMajor) || !Expect(text, ".") || !ReadNumber(text, version.mMinor) || !Expect(text, ".")
      || !ReadNumber(text, version.mPatch)) {
    return std::nullopt;
  }
  if (text.empty()) {
    return version;
  }
  if (!Expect(text, "-beta.") || !ReadNumber(text, version.mBeta) || version.mBeta == 0 || !text.empty()) {
    return std::nullopt;
  }
  return version;
}

std::string Version::ToString() const {
  auto text = std::format("{}.{}.{}", mMajor, mMinor, mPatch);
  if (mBeta != 0) {
    text += std::format("-beta.{}", mBeta);
  }
  return text;
}

std::strong_ordering Version::operator<=>(const Version& other) const noexcept {
  if (const auto order = mMajor <=> other.mMajor; order != 0) {
    return order;
  }
  if (const auto order = mMinor <=> other.mMinor; order != 0) {
    return order;
  }
  if (const auto order = mPatch <=> other.mPatch; order != 0) {
    return order;
  }
  return BetaRank(mBeta) <=> BetaRank(other.mBeta);
}

}// namespace OpenDesktopVR::Install
