// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Updates from the project's GitHub Releases (ADR-045): which release to
// offer. Releases are the stable channel; pre-releases are the beta, which
// the user opts into. Only a newer version than the running one is offered,
// never an older one (leaving the beta waits for the next release).
//
// The release list is GitHub's REST answer (GET /repos/{owner}/{repo}/
// releases), from outside (TH-9): bounded, parsed without exceptions, drafts
// and malformed entries skipped. A release needs a version tag ("v0.7.1",
// "v0.7.2-beta.1"), its setup program as an asset
// ("OpenDesktopVR-Setup-0.7.1.exe"), and that asset's SHA-256: GitHub's own
// digest, or a "<asset>.sha256" asset.
#pragma once

#include <OpenDesktopVR/Version.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Update {

inline constexpr std::size_t MaxReleaseListBytes = 4u << 20;
inline constexpr std::size_t MaxReleases = 100;
inline constexpr std::size_t MaxAssetsPerRelease = 50;
inline constexpr std::uint64_t MaxSetupBytes = 256ull << 20;
inline constexpr std::size_t MaxChecksumBytes = 4096;

struct Asset {
  std::string mName;
  std::string mUrl;// browser_download_url: https only
  std::uint64_t mSize {0};
  std::string mSha256;// from GitHub's "digest", if it gave one (64 lowercase hex)
};

struct Release {
  Install::Version mVersion;
  std::string mTag;
  std::string mPage;// html_url: the release notes
  bool mPrerelease {false};
  std::vector<Asset> mAssets;
};

// `allowLocalHttp`: tests only, "http://127.0.0.1" addresses kept (a fake GitHub)
std::vector<Release> ParseReleases(std::string_view json, std::string* error = nullptr, bool allowLocalHttp = false) noexcept;

struct Offer {
  Release mRelease;
  Asset mSetup;
  // Where its SHA-256 comes from when GitHub gave no digest
  std::optional<Asset> mChecksumFile;
};

// The newest release newer than `current` with a setup program and a way to
// check it: releases only, or pre-releases too (the beta)
std::optional<Offer> ChooseUpdate(std::span<const Release> releases, const Install::Version& current, bool beta) noexcept;

// "OpenDesktopVR-Setup-0.7.1.exe"
std::string SetupAssetName(const Install::Version& version);

// The hash for `name` in a checksum file: "<64 hex>  <name>" lines
// (sha256sum's format), or a lone hash. Lowercase.
std::optional<std::string> ChecksumFor(std::string_view text, std::string_view name) noexcept;

}// namespace OpenDesktopVR::Update
