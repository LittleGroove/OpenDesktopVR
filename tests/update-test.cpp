// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Updates from GitHub Releases (ADR-045), without the network:
// - a GitHub-shaped release list read: drafts, tags that aren't versions, a
//   beta not marked as a pre-release and non-https assets skipped;
// - the choice: releases only, or pre-releases too (the beta); never an
//   older or equal version; a release needs its setup program and a way to
//   check it (GitHub's digest or a .sha256 asset);
// - checksum files in sha256sum's format;
// - 5,000 random corruptions of the list never crash or yield anything
//   unsafe (TH-9);
// - the HTTPS client against a fake server on 127.0.0.1: answers, a
//   redirect, a 404, too big, cut short, a download with progress; plain
//   HTTP refused unless a test allows 127.0.0.1.
//
//   update-test

#include "FakeHttpServer.hpp"
#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Http.hpp>
#include <OpenDesktopVR/Releases.hpp>

#include <filesystem>
#include <format>
#include <random>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;
namespace fs = std::filesystem;

namespace {

const std::string Hash1(64, 'a');
const std::string Hash2 = std::string(63, 'B') + "C";

// The shape of GitHub's GET /repos/{owner}/{repo}/releases
std::string SampleList() {
  return std::format(R"([
  {{"tag_name": "v0.9.0", "draft": true, "prerelease": false, "html_url": "https://github.com/o/r/releases/tag/v0.9.0",
    "assets": [{{"name": "OpenDesktopVR-Setup-0.9.0.exe", "size": 100, "digest": "sha256:{0}",
                "browser_download_url": "https://github.com/o/r/releases/download/v0.9.0/OpenDesktopVR-Setup-0.9.0.exe"}}]}},
  {{"tag_name": "v0.8.0-beta.1", "draft": false, "prerelease": true, "html_url": "https://github.com/o/r/releases/tag/v0.8.0-beta.1",
    "assets": [{{"name": "OpenDesktopVR-Setup-0.8.0-beta.1.exe", "size": 2700000, "digest": "sha256:{1}",
                "browser_download_url": "https://github.com/o/r/releases/download/v0.8.0-beta.1/OpenDesktopVR-Setup-0.8.0-beta.1.exe"}}]}},
  {{"tag_name": "v0.7.2-beta.1", "draft": false, "prerelease": false,
    "assets": [{{"name": "OpenDesktopVR-Setup-0.7.2-beta.1.exe", "size": 100, "digest": "sha256:{0}",
                "browser_download_url": "https://github.com/o/r/releases/download/x/OpenDesktopVR-Setup-0.7.2-beta.1.exe"}}]}},
  {{"tag_name": "nightly", "draft": false, "prerelease": true, "assets": []}},
  {{"tag_name": "v0.7.1", "draft": false, "prerelease": false, "html_url": "https://github.com/o/r/releases/tag/v0.7.1",
    "assets": [{{"name": "OpenDesktopVR-Setup-0.7.1.exe", "size": 2600000,
                "browser_download_url": "https://github.com/o/r/releases/download/v0.7.1/OpenDesktopVR-Setup-0.7.1.exe"}},
               {{"name": "OpenDesktopVR-Setup-0.7.1.exe.sha256", "size": 99,
                "browser_download_url": "https://github.com/o/r/releases/download/v0.7.1/OpenDesktopVR-Setup-0.7.1.exe.sha256"}},
               {{"name": "notes.txt", "size": 5, "browser_download_url": "http://insecure.example/notes.txt"}}]}},
  {{"tag_name": "v0.7.0", "draft": false, "prerelease": false,
    "assets": [{{"name": "OpenDesktopVR-Setup-0.7.0.exe", "size": 2500000, "digest": "sha256:{0}",
                "browser_download_url": "https://github.com/o/r/releases/download/v0.7.0/OpenDesktopVR-Setup-0.7.0.exe"}}]}},
  {{"tag_name": "v0.6.9", "draft": false, "prerelease": false,
    "assets": [{{"name": "OpenDesktopVR-Setup-0.6.9.exe", "size": 2500000,
                "browser_download_url": "https://github.com/o/r/releases/download/v0.6.9/OpenDesktopVR-Setup-0.6.9.exe"}}]}}
])", Hash1, Hash2);
}

Install::Version V(std::string_view text) {
  return *Install::ParseVersion(text);
}

}// namespace

int wmain() {
  std::mt19937 random {45};

  std::vector<Update::Release> releases;
  Scenario("A GitHub release list read; drafts and nonsense skipped (TH-9)", [&] {
    std::string error;
    releases = Update::ParseReleases(SampleList(), &error);
    Check(error.empty(), "read without an error " + error);
    Check(releases.size() == 4, std::format("4 usable releases ({}): no draft, no 'nightly', no unmarked beta", releases.size()));
    if (releases.size() != 4) {
      return;
    }
    Check(releases[0].mVersion == V("0.8.0-beta.1") && releases[0].mPrerelease, "0.8.0-beta.1, a pre-release");
    Check(releases[0].mAssets.size() == 1 && releases[0].mAssets[0].mSha256 == std::string(63, 'b') + "c",
      "its asset's digest, in lowercase");
    Check(releases[1].mVersion == V("0.7.1") && releases[1].mAssets.size() == 2 && releases[1].mAssets[0].mSha256.empty(),
      "0.7.1: its setup program and its .sha256 file (the http asset skipped)");
    Check(releases[1].mPage == "https://github.com/o/r/releases/tag/v0.7.1", "its notes page");
  });

  Scenario("The choice: releases, or pre-releases too (the beta); never older", [&] {
    const auto stable = Update::ChooseUpdate(releases, V("0.7.0"), false);
    Check(stable && stable->mRelease.mVersion == V("0.7.1") && stable->mSetup.mName == "OpenDesktopVR-Setup-0.7.1.exe"
        && stable->mChecksumFile && stable->mChecksumFile->mName == "OpenDesktopVR-Setup-0.7.1.exe.sha256",
      "on 0.7.0, releases only: 0.7.1, checked by its .sha256 file");
    const auto beta = Update::ChooseUpdate(releases, V("0.7.0"), true);
    Check(beta && beta->mRelease.mVersion == V("0.8.0-beta.1") && !beta->mChecksumFile && beta->mSetup.mSha256.size() == 64,
      "on 0.7.0 with the beta: 0.8.0-beta.1, checked by GitHub's digest");
    Check(!Update::ChooseUpdate(releases, V("0.8.0-beta.1"), false), "on the beta, then leaving it: 0.7.1 is older, so nothing");
    Check(!Update::ChooseUpdate(releases, V("0.8.0-beta.1"), true), "on the newest beta: nothing");
    Check(!Update::ChooseUpdate(releases, V("0.7.1"), false), "on 0.7.1: nothing (the same version isn't offered)");
    const auto older = Update::ChooseUpdate(releases, V("0.6.9"), false);
    Check(older && older->mRelease.mVersion == V("0.7.1"), "on 0.6.9: the newest release, 0.7.1");

    auto noSetup = releases;
    noSetup[1].mAssets.erase(noSetup[1].mAssets.begin());
    const auto fallback = Update::ChooseUpdate(noSetup, V("0.6.0"), false);
    Check(fallback && fallback->mRelease.mVersion == V("0.7.0"), "0.7.1 without its setup program: 0.7.0 instead");
    auto unchecked = releases;
    unchecked[1].mAssets.pop_back();
    Check(!Update::ChooseUpdate(unchecked, V("0.7.0"), false), "0.7.1 with no digest and no .sha256 file: not offered");
    auto huge = releases;
    huge[1].mAssets[0].mSize = Update::MaxSetupBytes + 1;
    Check(!Update::ChooseUpdate(huge, V("0.7.0"), false), "a setup program over the size limit: not offered");
  });

  Scenario("Checksum files (sha256sum's format)", [&] {
    const std::string name = "OpenDesktopVR-Setup-0.7.1.exe";
    Check(Update::ChecksumFor(Hash1 + "  " + name + "\n", name) == Hash1, "'<hash>  <name>'");
    Check(Update::ChecksumFor(Hash2 + " *" + name + "\r\n", name) == std::string(63, 'b') + "c", "'<hash> *<name>', in lowercase");
    Check(Update::ChecksumFor(Hash1 + "\n", name) == Hash1, "a lone hash");
    Check(Update::ChecksumFor(Hash2 + "  other.exe\n" + Hash1 + "  " + name + "\n", name) == Hash1, "the right line of several");
    Check(!Update::ChecksumFor(Hash1 + "  other.exe\n", name), "another file's hash: none");
    Check(!Update::ChecksumFor("xyz  " + name, name), "not a hash: none");
    Check(!Update::ChecksumFor(std::string(Update::MaxChecksumBytes + 1, 'a'), name), "too big: none");
  });

  Scenario("Nonsense refused; 5,000 random corruptions never crash or yield anything unsafe (TH-9)", [&] {
    // Parsed first, then reported (a call's arguments may be made in any order)
    const auto refuse = [](std::string_view json, std::string_view what) {
      std::string error;
      const bool refused = Update::ParseReleases(json, &error).empty() && !error.empty();
      Check(refused, std::format("{}: {}", what, error));
    };
    refuse("not json", "not JSON");
    refuse(R"({"message": "Not Found"})", "an object, not a list");
    refuse(std::string(Update::MaxReleaseListBytes + 1, ' '), "too big");
    const auto sample = SampleList();
    int offered = 0;
    int unsafe = 0;
    for (int i = 0; i < 5'000; ++i) {
      auto damaged = sample;
      for (int c = 0; c < 1 + static_cast<int>(random() % 6); ++c) {
        damaged[random() % damaged.size()] = static_cast<char>(random() % 128);
      }
      const auto parsed = Update::ParseReleases(damaged);
      for (const auto& release: parsed) {
        for (const auto& asset: release.mAssets) {
          unsafe += asset.mUrl.starts_with("https://") ? 0 : 1;
        }
      }
      if (const auto offer = Update::ChooseUpdate(parsed, V("0.7.0"), true)) {
        ++offered;
        // Asset names match ignoring case (Windows' file names do)
        const auto lower = [](std::string text) {
          for (auto& c: text) {
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
          }
          return text;
        };
        unsafe += lower(offer->mSetup.mName) == lower(Update::SetupAssetName(offer->mRelease.mVersion))
            && V("0.7.0") < offer->mRelease.mVersion
          ? 0
          : 1;
      }
    }
    Check(unsafe == 0,
      std::format("nothing unsafe: every asset https; every offer newer, its setup named for its version ({} offers)", offered));
  });

  Scenario("The HTTPS client against a fake server: answers, a redirect, refusals", [&] {
    const std::string big(300'000, 'x');
    FakeHttpServer server {[&](const std::string& path) -> FakeHttpServer::Response {
      if (path == "/list") {
        return {200, "[]"};
      }
      if (path == "/moved") {
        return {302, "", "/list"};
      }
      if (path == "/big") {
        return {200, big};
      }
      if (path == "/short") {
        return {200, "partial", "", true};
      }
      return {404, "missing"};
    }};
    Update::HttpOptions local;
    local.mAllowLocalHttp = true;
    local.mTimeout = std::chrono::milliseconds(5'000);
    const auto ok = Update::Get(server.Url("/list"), local);
    Check(ok.mOk && ok.mStatus == 200 && ok.mBody == "[]", "an answer: " + ok.mError);
    const auto moved = Update::Get(server.Url("/moved"), local);
    Check(moved.mOk && moved.mBody == "[]" && server.Requests("/list") == 2, "a redirect followed");
    const auto missing = Update::Get(server.Url("/nothing"), local);
    Check(!missing.mOk && missing.mStatus == 404, "a 404: " + missing.mError);
    auto small = local;
    small.mMaxBytes = 1000;
    const auto tooBig = Update::Get(server.Url("/big"), small);
    Check(!tooBig.mOk, "over its size limit: refused: " + tooBig.mError);
    const auto cut = Update::Get(server.Url("/short"), local);
    Check(!cut.mOk, "cut short: refused: " + cut.mError);
    const auto plain = Update::Get(server.Url("/list"), Update::HttpOptions {});
    Check(!plain.mOk && server.Requests("/list") == 2, "plain http without the test's permission: refused, never sent: " + plain.mError);
    const auto elsewhere = Update::Get("http://example.com/x", local);
    Check(!elsewhere.mOk, "plain http to anywhere but 127.0.0.1: refused: " + elsewhere.mError);
    Check(!Update::Get("ftp://127.0.0.1/x", local).mOk && !Update::Get("nonsense", local).mOk, "other schemes and nonsense: refused");

    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"download.bin";
    int calls = 0;
    auto large = local;
    large.mMaxBytes = big.size();
    const auto download = Update::Download(server.Url("/big"), file, large, [&](std::uint64_t, std::uint64_t total) {
      ++calls;
      return total == big.size();
    });
    Check(download.mOk && FileUtil::ReadAll(file) == big && calls > 0, std::format("a download, with progress ({} calls)", calls));
    const fs::path failed = fs::path {folder.Path()} / L"failed.bin";
    Check(!Update::Download(server.Url("/short"), failed, local).mOk && !fs::exists(failed), "a failed download leaves no file");
  });

  return Finish();
}
