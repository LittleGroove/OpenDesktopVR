// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The program's versions (ADR-043):
// - "0.7.0" is a release: the stable channel;
// - "0.7.1-beta.2" is a pre-release, the beta channel. It comes before the
//   release with the same numbers, and after "0.7.1-beta.1".
// A leading "v" (a Git tag's) is allowed. Release names come from outside
// (GitHub, M7b), so parsing is bounded and never throws (TH-9).
#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace OpenDesktopVR::Install {

inline constexpr unsigned MaxVersionPart = 99'999;

struct Version {
  unsigned mMajor {0};
  unsigned mMinor {0};
  unsigned mPatch {0};
  // 0: a release; N: "-beta.N", before the release of the same numbers
  unsigned mBeta {0};

  bool IsBeta() const noexcept {
    return mBeta != 0;
  }
  std::string ToString() const;
  std::strong_ordering operator<=>(const Version& other) const noexcept;
  bool operator==(const Version& other) const noexcept = default;
};

// "0.7.0", "v0.7.0", "0.7.1-beta.2"; nullopt for anything else
std::optional<Version> ParseVersion(std::string_view text) noexcept;

}// namespace OpenDesktopVR::Install
