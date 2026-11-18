// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/Releases.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>

namespace OpenDesktopVR::Update {

namespace {

constexpr std::size_t MaxTextBytes = 2048;

bool IsHex64(std::string_view text) noexcept {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

std::string Lower(std::string_view text) {
  std::string lower;
  for (const char c: text) {
    lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  return lower;
}

// A bounded string field; empty if missing, not a string or too long
std::string Text(const JsonUtil::Json& object, std::string_view key) {
  auto value = JsonUtil::GetOr<std::string>(object, key, "");
  return value.size() > MaxTextBytes ? std::string {} : value;
}

bool IsHttps(std::string_view url) noexcept {
  return url.starts_with("https://") && url.size() > 8;
}

std::optional<Asset> ReadAsset(const JsonUtil::Json& entry, bool allowLocalHttp) {
  Asset asset;
  asset.mName = Text(entry, "name");
  asset.mUrl = Text(entry, "browser_download_url");
  asset.mSize = JsonUtil::GetOr<std::uint64_t>(entry, "size", 0);
  const bool usable = IsHttps(asset.mUrl) || (allowLocalHttp && asset.mUrl.starts_with("http://127.0.0.1:"));
  if (asset.mName.empty() || !usable) {
    return std::nullopt;
  }
  // GitHub's digest: "sha256:<hex>"
  const auto digest = Text(entry, "digest");
  if (digest.starts_with("sha256:") && IsHex64(std::string_view {digest}.substr(7))) {
    asset.mSha256 = Lower(std::string_view {digest}.substr(7));
  }
  return asset;
}

const Asset* FindAsset(const Release& release, std::string_view name) {
  for (const auto& asset: release.mAssets) {
    if (StringUtil::EqualsIgnoreCase(StringUtil::Widen(asset.mName), StringUtil::Widen(name))) {
      return &asset;
    }
  }
  return nullptr;
}

}// namespace

std::string SetupAssetName(const Install::Version& version) {
  return "OpenDesktopVR-Setup-" + version.ToString() + ".exe";
}

std::vector<Release> ParseReleases(std::string_view json, std::string* error, bool allowLocalHttp) noexcept {
  const auto fail = [&](std::string_view why) {
    if (error) {
      try {
        *error = why;
      } catch (...) {
      }
    }
    return std::vector<Release> {};
  };
  try {
    if (json.size() > MaxReleaseListBytes) {
      return fail("the release list is too big");
    }
    const auto list = JsonUtil::Parse(json);
    if (!list || !list->is_array()) {
      return fail("the release list isn't readable");
    }
    std::vector<Release> releases;
    for (const auto& entry: *list) {
      if (releases.size() >= MaxReleases) {
        break;
      }
      if (!entry.is_object() || JsonUtil::GetOr<bool>(entry, "draft", false)) {
        continue;
      }
      Release release;
      release.mTag = Text(entry, "tag_name");
      const auto version = Install::ParseVersion(release.mTag);
      if (!version) {
        continue;
      }
      release.mVersion = *version;
      release.mPrerelease = JsonUtil::GetOr<bool>(entry, "prerelease", false);
      // A pre-release must be marked so, and named so: never a beta on the
      // stable channel by mistake
      if (release.mVersion.IsBeta() && !release.mPrerelease) {
        continue;
      }
      release.mPage = Text(entry, "html_url");
      if (!IsHttps(release.mPage)) {
        release.mPage.clear();
      }
      if (const auto assets = JsonUtil::Get<JsonUtil::Json>(entry, "assets"); assets && assets->is_array()) {
        for (const auto& item: *assets) {
          if (release.mAssets.size() >= MaxAssetsPerRelease) {
            break;
          }
          if (auto asset = ReadAsset(item, allowLocalHttp)) {
            release.mAssets.push_back(std::move(*asset));
          }
        }
      }
      releases.push_back(std::move(release));
    }
    return releases;
  } catch (...) {
    return fail("the release list couldn't be read");
  }
}

std::optional<Offer> ChooseUpdate(std::span<const Release> releases, const Install::Version& current, bool beta) noexcept {
  try {
    const Release* best = nullptr;
    Offer offer;
    for (const auto& release: releases) {
      if ((release.mPrerelease && !beta) || !(current < release.mVersion) || (best && !(best->mVersion < release.mVersion))) {
        continue;
      }
      const auto name = SetupAssetName(release.mVersion);
      const auto* setup = FindAsset(release, name);
      if (!setup || setup->mSize == 0 || setup->mSize > MaxSetupBytes) {
        continue;
      }
      const auto* checksum = FindAsset(release, name + ".sha256");
      if (setup->mSha256.empty() && !checksum) {
        continue;// nothing to check it by
      }
      best = &release;
      offer.mRelease = release;
      offer.mSetup = *setup;
      offer.mChecksumFile = checksum && setup->mSha256.empty() ? std::optional<Asset> {*checksum} : std::nullopt;
    }
    return best ? std::optional<Offer> {std::move(offer)} : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> ChecksumFor(std::string_view text, std::string_view name) noexcept {
  try {
    if (text.size() > MaxChecksumBytes) {
      return std::nullopt;
    }
    std::size_t start = 0;
    std::optional<std::string> lone;
    int lines = 0;
    while (start < text.size()) {
      auto end = text.find('\n', start);
      end = end == std::string_view::npos ? text.size() : end;
      auto line = text.substr(start, end - start);
      start = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
      }
      if (line.empty()) {
        continue;
      }
      ++lines;
      if (line.size() < 64 || !IsHex64(line.substr(0, 64))) {
        return std::nullopt;
      }
      const auto hash = Lower(line.substr(0, 64));
      auto rest = line.substr(64);
      if (rest.empty()) {
        lone = hash;
        continue;
      }
      if (rest.front() != ' ' && rest.front() != '\t') {
        return std::nullopt;
      }
      while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' || rest.front() == '*')) {
        rest.remove_prefix(1);
      }
      if (StringUtil::EqualsIgnoreCase(StringUtil::Widen(rest), StringUtil::Widen(name))) {
        return hash;
      }
    }
    return lines == 1 ? lone : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

}// namespace OpenDesktopVR::Update
