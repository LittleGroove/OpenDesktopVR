// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Diagnostics and support (M6c): the .zip writer (read back here, and by
// Windows' own tar), path redaction, the "doesn't show in game" self-test's
// verdicts (UI-10, LG-5; CMP-1, CMP-2, CMP-3, CMP-7, CMP-8), the debug
// bundle from a made-up data folder (LG-4), and the system description.
//
//   diagnostics-test

#include "check.hpp"

#include <OpenDesktopVR/DebugBundle.hpp>
#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/KnownLayers.hpp>
#include <OpenDesktopVR/Redaction.hpp>
#include <OpenDesktopVR/SelfTest.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/SystemInfo.hpp>
#include <OpenDesktopVR/ZipWriter.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <string>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Diagnostics;
using Testing::Check;
using Testing::Scenario;
namespace fs = std::filesystem;

namespace {

uint32_t Get16(const std::string& bytes, std::size_t at) {
  return static_cast<unsigned char>(bytes[at]) | (static_cast<unsigned char>(bytes[at + 1]) << 8);
}
uint32_t Get32(const std::string& bytes, std::size_t at) {
  return Get16(bytes, at) | (Get16(bytes, at + 2) << 16);
}

// Reads a stored-only archive: name -> data. Empty if anything is off,
// including a CRC that doesn't match.
std::map<std::string, std::string> ReadZip(const std::string& bytes, std::string& problem) {
  std::map<std::string, std::string> files;
  if (bytes.size() < 22 || Get32(bytes, bytes.size() - 22) != 0x06054B50) {
    problem = "no end record";
    return {};
  }
  const auto end = bytes.size() - 22;
  const auto count = Get16(bytes, end + 10);
  auto at = static_cast<std::size_t>(Get32(bytes, end + 16));
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 46 > bytes.size() || Get32(bytes, at) != 0x02014B50) {
      problem = "bad central entry";
      return {};
    }
    const auto crc = Get32(bytes, at + 16);
    const auto size = Get32(bytes, at + 20);
    const auto nameLength = Get16(bytes, at + 28);
    const auto extra = Get16(bytes, at + 30);
    const auto comment = Get16(bytes, at + 32);
    const auto local = static_cast<std::size_t>(Get32(bytes, at + 42));
    const auto name = bytes.substr(at + 46, nameLength);
    if (Get32(bytes, local) != 0x04034B50 || Get16(bytes, local + 8) != 0) {
      problem = "bad local header";
      return {};
    }
    const auto dataAt = local + 30 + Get16(bytes, local + 26) + Get16(bytes, local + 28);
    auto data = bytes.substr(dataAt, size);
    if (Crc32(data) != crc) {
      problem = std::format("CRC mismatch in {}", name);
      return {};
    }
    files[name] = std::move(data);
    at += 46 + nameLength + extra + comment;
  }
  return files;
}

// Windows' tar (bsdtar, Windows 10 1803 and later) lists the archive
std::optional<std::string> TarList(const fs::path& zip) {
  wchar_t system[MAX_PATH] {};
  GetSystemDirectoryW(system, MAX_PATH);
  const fs::path tar = fs::path {system} / L"tar.exe";
  if (!fs::exists(tar)) {
    return std::nullopt;
  }
  SECURITY_ATTRIBUTES inherit {sizeof(inherit), nullptr, TRUE};
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  CreatePipe(&read, &write, &inherit, 0);
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup {sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write;
  startup.hStdError = write;
  PROCESS_INFORMATION process {};
  auto command = std::format(L"\"{}\" -tf \"{}\"", tar.wstring(), zip.wstring());
  const bool started = CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(write);
  std::string output;
  if (started) {
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
      output.append(buffer, got);
    }
    WaitForSingleObject(process.hProcess, 10'000);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
  }
  CloseHandle(read);
  return output;
}

Registration::LayerEntry Entry(std::wstring path, std::string name, bool enabled = true, bool ours = false) {
  Registration::LayerEntry entry;
  entry.mRoot = HKEY_CURRENT_USER;
  entry.mWhere = L"HKCU (64-bit)";
  entry.mManifestPath = path;
  entry.mEnabled = enabled;
  entry.mManifestExists = true;
  entry.mLayerName = std::move(name);
  entry.mLibraryPath = "layer.dll";
  entry.mIsOurs = ours;
  return entry;
}

const Diagnostics::SelfCheck* Find(const std::vector<Diagnostics::SelfCheck>& checks, CheckID id) {
  const auto it = std::ranges::find_if(checks, [id](const Diagnostics::SelfCheck& check) { return check.mID == id; });
  return it == checks.end() ? nullptr : &*it;
}

bool Is(const std::vector<Diagnostics::SelfCheck>& checks, CheckID id, Verdict verdict) {
  const auto* check = Find(checks, id);
  return check && check->mVerdict == verdict;
}

}// namespace

int main() {
  Testing::TempFolder temp;

  Scenario("CRC-32 and a stored .zip that reads back, here and in Windows' tar", [&] {
    Check(Crc32("123456789") == 0xCBF43926u, "CRC-32 of the standard check string");
    Check(Crc32("") == 0, "of nothing: 0");
    ZipWriter zip;
    std::string binary(3000, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i) {
      binary[i] = static_cast<char>(i * 7);
    }
    Check(zip.Add("README.txt", "hello\r\n") && zip.Add("logs/host.log", "a log line") && zip.Add("crashdumps/x.dmp", binary), "three files added");
    Check(!zip.Add("", "x"), "a file needs a name");
    const auto bytes = zip.Finish();
    std::string problem;
    const auto files = ReadZip(bytes, problem);
    Check(problem.empty(), std::format("the archive parses, CRCs match ({})", problem.empty() ? "ok" : problem));
    Check(files.size() == 3 && files.at("logs/host.log") == "a log line" && files.at("crashdumps/x.dmp") == binary, "names and contents survive");
    Check(zip.FileCount() == 0 && zip.Bytes() == 0, "the writer is empty after Finish");

    const auto path = fs::path {temp.Path()} / L"test.zip";
    FileUtil::WriteAtomic(path, bytes);
    if (const auto listing = TarList(path)) {
      Check(listing->find("logs/host.log") != std::string::npos && listing->find("crashdumps/x.dmp") != std::string::npos,
        std::format("Windows' tar lists it ({} bytes of output)", listing->size()));
    } else {
      std::printf("  (no tar.exe here: skipped)\n");
    }
  });

  Scenario("Paths lose the user's name; other words don't", [] {
    const auto redaction = Redaction::ForProfile("C:\\Users\\Robin");
    Check(Redact("C:\\Users\\Robin\\AppData\\Local\\x.log", redaction) == "%USERPROFILE%\\AppData\\Local\\x.log", "the profile folder");
    Check(Redact("c:\\users\\robin\\Desktop", redaction) == "%USERPROFILE%\\Desktop", "in any case");
    Check(Redact(R"({"path": "C:\\Users\\Robin\\Music"})", redaction) == R"({"path": "%USERPROFILE%\\Music"})", "JSON's doubled backslashes");
    Check(Redact("C:/Users/Robin/Videos", redaction) == "%USERPROFILE%/Videos", "forward slashes");
    Check(Redact("D:\\Users\\Someone\\Games", redaction) == "D:\\Users\\<user>\\Games", "another user's folder");
    Check(Redact("C:\\Users\\Public\\Documents", redaction) == "C:\\Users\\Public\\Documents", "Public stays");
    Check(Redact("Robin started the game", redaction) == "Robin started the game", "a bare word stays");
    Check(Redact("UsersGuide.txt and \\Users", redaction) == "UsersGuide.txt and \\Users", "no path, nothing changed");
  });

  const fs::path ourManifest = L"C:\\Program Files\\OpenDesktopVR\\OpenDesktopVR-Layer.json";
  const auto healthy = [&] {
    SelfTestInput input;
    input.mLayers = {Entry(ourManifest.wstring(), "XR_APILAYER_ours", true, true)};
    input.mOurManifest = ourManifest;
    input.mUacOn = true;
    input.mRecenterBound = true;
    return input;
  };
  const auto withGame = [](SelfTestInput input, uint32_t flags, uint32_t known = 0, uint32_t nearest = 0) {
    input.mGame = GameSeen {"EliteDangerous64.exe", true, std::chrono::seconds {200}, ProcessUtil::Elevation::NotElevated, flags, 0, known, nearest};
    return input;
  };

  Scenario("Self-test: our layer (UI-10, CMP-3)", [&] {
    auto checks = RunSelfTest(healthy());
    Check(Is(checks, CheckID::Registered, Verdict::Pass) && Is(checks, CheckID::Manifest, Verdict::Pass), "registered, manifest readable: ok");
    Check(!Find(checks, CheckID::OtherCopies), "no other copy: nothing said");
    Check(Is(checks, CheckID::OnInSettings, Verdict::Pass), "on in settings");

    auto missing = healthy();
    missing.mLayers.clear();
    checks = RunSelfTest(missing);
    Check(Is(checks, CheckID::Registered, Verdict::Fail) && !Find(checks, CheckID::Manifest), "not registered: a problem, with a fix");
    Check(!Find(checks, CheckID::Registered)->mFix.empty(), "the fix is given");

    auto off = healthy();
    off.mLayers[0].mEnabled = false;
    Check(Is(RunSelfTest(off), CheckID::Registered, Verdict::Fail), "registered but off: a problem");

    auto gone = healthy();
    gone.mLayers[0].mManifestExists = false;
    gone.mLayers[0].mLayerName.reset();
    Check(Is(RunSelfTest(gone), CheckID::Manifest, Verdict::Fail), "manifest missing: a problem");

    auto twice = healthy();
    twice.mLayers.push_back(Entry(L"C:\\Old\\OpenDesktopVR-Layer.json", "XR_APILAYER_ours", true, true));
    Check(Is(RunSelfTest(twice), CheckID::OtherCopies, Verdict::Warning), "another copy of ours on: a warning (CMP-3)");
    twice.mLayers[1].mEnabled = false;
    Check(!Find(RunSelfTest(twice), CheckID::OtherCopies), "the other copy off: fine");

    auto settingsOff = healthy();
    settingsOff.mLayerOnInSettings = false;
    Check(Is(RunSelfTest(settingsOff), CheckID::OnInSettings, Verdict::Fail), "off in settings: a problem");
    auto gameOff = withGame(healthy(), SharedAbi::ChainKnown);
    gameOff.mDisabledGames = {L"elitedangerous64.exe"};
    Check(Is(RunSelfTest(gameOff), CheckID::OnInSettings, Verdict::Fail), "off for this game: a problem");
  });

  Scenario("Self-test: a game, and when", [&] {
    Check(Is(RunSelfTest(healthy()), CheckID::GameLoaded, Verdict::Warning), "no game yet: a warning, with advice");
    const auto checks = RunSelfTest(withGame(healthy(), SharedAbi::ChainKnown));
    const auto* game = Find(checks, CheckID::GameLoaded);
    Check(game && game->mVerdict == Verdict::Pass && game->mDetail.find("EliteDangerous64.exe loaded it 3 minutes ago") != std::string::npos,
      std::format("the game and when ({})", game ? game->mDetail : ""));
    Check(DescribeAgo(std::chrono::seconds {5}) == "just now" && DescribeAgo(std::chrono::seconds {7200}) == "2 hours ago", "times read naturally");
  });

  Scenario("Self-test: layer order, from what the layer saw (CMP-1, CMP-2)", [&] {
    using namespace KnownLayers;
    using SharedAbi::ChainKnown;
    auto input = healthy();
    input.mLayers.push_back(Entry(L"C:\\Ultraleap\\UltraleapHandTracking.json", "XR_APILAYER_ULTRALEAP_hand_tracking"));
    input.mLayers.push_back(Entry(L"C:\\XRNeckSafer\\XRNeckSafer.json", "XR_APILAYER_NOVENDOR_XRNeckSafer"));
    Check(Is(RunSelfTest(input), CheckID::Ultraleap, Verdict::Info), "no game yet: Ultraleap's place can't be told (a note)");
    Check(Is(RunSelfTest(withGame(input, ChainKnown, Ultraleap | PoseChanging, Ultraleap)), CheckID::Ultraleap, Verdict::Pass), "Ultraleap last: ok");
    Check(Is(RunSelfTest(withGame(input, ChainKnown, Ultraleap | PoseChanging, PoseChanging)), CheckID::Ultraleap, Verdict::Warning), "Ultraleap below but not last: a warning");
    const auto above = RunSelfTest(withGame(input, ChainKnown, PoseChanging, PoseChanging));
    Check(Is(above, CheckID::Ultraleap, Verdict::Fail), "Ultraleap above ours: a problem");
    Check(Find(above, CheckID::Ultraleap)->mFix.find("never changes other products' settings") != std::string::npos, "advice only (CMP-2)");
    Check(Is(above, CheckID::PoseChanging, Verdict::Pass), "XRNeckSafer below ours: ok");
    Check(Is(RunSelfTest(withGame(input, ChainKnown, Ultraleap, Ultraleap)), CheckID::PoseChanging, Verdict::Warning), "XRNeckSafer above ours: a warning");
    input.mLayers[1].mEnabled = false;
    Check(!Find(RunSelfTest(input), CheckID::Ultraleap), "Ultraleap turned off: nothing said");
    Check(!Find(RunSelfTest(withGame(input, 0)), CheckID::Depth), "a layer from before M6c: no depth verdict");
  });

  Scenario("Self-test: depth, elevation, UAC, recenter (CMP-7, CMP-8)", [&] {
    using SharedAbi::ChainKnown;
    using SharedAbi::DepthEnabled;
    Check(Is(RunSelfTest(withGame(healthy(), ChainKnown | DepthEnabled)), CheckID::Depth, Verdict::Warning), "depth submitted: a warning (CMP-7)");
    Check(Is(RunSelfTest(withGame(healthy(), ChainKnown)), CheckID::Depth, Verdict::Pass), "no depth: ok");

    auto elevated = healthy();
    elevated.mHost = ProcessUtil::Elevation::Elevated;
    Check(Is(RunSelfTest(elevated), CheckID::HostElevation, Verdict::Warning), "this app elevated: a warning");
    auto explorer = healthy();
    explorer.mExplorer = ProcessUtil::Elevation::Elevated;
    Check(Is(RunSelfTest(explorer), CheckID::ExplorerElevation, Verdict::Warning), "Explorer elevated: a warning");
    Check(!Find(RunSelfTest(healthy()), CheckID::ExplorerElevation), "Explorer normal: nothing said");
    auto uac = healthy();
    uac.mUacOn = false;
    Check(Is(RunSelfTest(uac), CheckID::Uac, Verdict::Warning), "UAC off: a warning");

    auto game = withGame(healthy(), ChainKnown);
    Check(Is(RunSelfTest(game), CheckID::GameElevation, Verdict::Pass), "game and app alike: ok");
    game.mGame->mElevation = ProcessUtil::Elevation::Elevated;
    Check(Is(RunSelfTest(game), CheckID::GameElevation, Verdict::Warning), "the game elevated, this app not: a warning");
    game.mGame->mElevation = ProcessUtil::Elevation::Unknown;
    const auto unknown = RunSelfTest(game);
    Check(Is(unknown, CheckID::GameElevation, Verdict::Warning) && Find(unknown, CheckID::GameElevation)->mDetail.find("probably") != std::string::npos,
      "can't be read: probably elevated");
    game.mGame->mRunning = false;
    Check(!Find(RunSelfTest(game), CheckID::GameElevation), "the game has ended: not checked");

    Check(Find(RunSelfTest(healthy()), CheckID::Recenter)->mFix.empty(), "recenter bound: no hint");
    auto unbound = healthy();
    unbound.mRecenterBound = false;
    Check(!Find(RunSelfTest(unbound), CheckID::Recenter)->mFix.empty(), "not bound: the hint to bind it");
    const auto text = FormatChecks(RunSelfTest(unbound));
    Check(text.find("[ok] The layer is registered") != std::string::npos && text.find("Fix: ") != std::string::npos, "as text for the bundle");
  });

  Scenario("The debug bundle (LG-4): what's in it, redacted, bounded", [&] {
    const fs::path data = fs::path {temp.Path()} / L"data";
    const std::string profile = "C:\\Users\\Robin";
    FileUtil::WriteAtomic(data / L"settings.json", R"({"panels": [{"window": "C:\\Users\\Robin\\notes.txt"}]})");
    FileUtil::WriteAtomic(data / L"profiles" / L"default.json", R"({"name": "Default"})");
    FileUtil::WriteAtomic(data / L"logs" / L"host.log", "Started from C:\\Users\\Robin\\Desktop\r\n");
    for (int i = 0; i < 12; ++i) {
      FileUtil::WriteAtomic(data / L"logs" / std::format(L"layer-game{}.log", i), "layer log");
    }
    FileUtil::WriteAtomic(data / L"crashdumps" / L"crashes.txt", "a crash\r\n");
    const std::string dump {"MDMP\0\x01\x02 C:\\Users\\Robin", 22};
    for (int i = 0; i < 7; ++i) {
      FileUtil::WriteAtomic(data / L"crashdumps" / std::format(L"OpenDesktopVR-{}.dmp", i), dump);
    }
    BundleSources sources;
    sources.mDataFolder = data;
    sources.mRedaction = Redaction::ForProfile(profile);
    sources.mTexts = {{"system.txt", "Windows: test"}, {"openxr-layers.txt", DescribeLayers({Entry(L"C:\\Users\\Robin\\x.json", "XR_APILAYER_ULTRALEAP_hand_tracking")})}};
    const auto zipPath = data / L"bundle.zip";
    const auto result = WriteBundle(zipPath, sources);
    Check(result.mOk, std::format("written ({} files, {} bytes{})", result.mFiles, result.mBytes, result.mError));
    std::string problem;
    const auto files = ReadZip(FileUtil::ReadAll(zipPath).value_or(""), problem);
    Check(problem.empty() && files.contains("README.txt") && files.contains("system.txt"), "it reads back, with a README and the texts");
    Check(files.contains("settings/settings.json") && files.contains("settings/profiles/default.json"), "settings and profiles");
    Check(files.contains("logs/host.log") && files.contains("crashdumps/crashes.txt"), "logs and the crash list");
    const auto count = [&files](std::string_view prefix, std::string_view suffix) {
      return std::ranges::count_if(files, [&](const auto& file) { return file.first.starts_with(prefix) && file.first.ends_with(suffix); });
    };
    Check(count("logs/layer-", ".log") == 10, std::format("the newest 10 layer logs of 12 ({})", count("logs/layer-", ".log")));
    Check(count("crashdumps/", ".dmp") == 5, std::format("the newest 5 crash dumps of 7 ({})", count("crashdumps/", ".dmp")));
    Check(files.contains("settings/settings.json") && files.at("settings/settings.json").find("Robin") == std::string::npos
        && files.at("settings/settings.json").find("%USERPROFILE%") != std::string::npos,
      "settings: the user's name is gone from paths");
    Check(files.contains("logs/host.log") && files.at("logs/host.log").find("Robin") == std::string::npos, "logs: likewise");
    const auto firstDump = std::ranges::find_if(files, [](const auto& file) { return file.first.ends_with(".dmp"); });
    Check(firstDump != files.end() && firstDump->second == dump, "dumps are as written (binary, not redacted)");
    Check(files.contains("openxr-layers.txt") && files.at("openxr-layers.txt").find("library: layer.dll") != std::string::npos
        && files.at("openxr-layers.txt").find("Ultraleap hand tracking") != std::string::npos,
      "every layer with its DLL and kind (LG-4)");
    Check(!PrivacyNotice().empty() && PrivacyNotice().find("crash dumps") != std::string_view::npos, "a privacy notice exists and mentions dumps");
    Check(SuggestedBundleName().starts_with(L"OpenDesktopVR-debug-") && SuggestedBundleName().ends_with(L".zip"), "a dated file name");

    BundleSources empty;
    const auto nothing = WriteBundle(fs::path {temp.Path()} / L"empty.zip", empty);
    Check(nothing.mOk && nothing.mFiles == 1, "no data folder: just the README");
    const auto bad = WriteBundle(fs::path {L"Z:\\no\\such\\drive\\x.zip"}, empty);
    Check(!bad.mOk && !bad.mError.empty(), "an unwritable place: an error, not a crash");
  });

  Scenario("The system, described (doc 09: versions, GPU, environment)", [] {
    const auto text = DescribeSystem();
    Check(text.find("Windows: ") != std::string::npos && text.find("Open Desktop VR: ") != std::string::npos, "versions");
    Check(text.find("GPU: ") != std::string::npos, "at least one GPU (WARP counts)");
    Check(text.find("Hardware-accelerated GPU scheduling: ") != std::string::npos, "GPU scheduling");
    Check(text.find("Code pages: ") != std::string::npos, "the code page");
    Check(WindowsVersion().starts_with("Windows"), std::format("Windows: {}", WindowsVersion()));
    const auto environment = CheckEnvironment();
    Check(!environment.mFacts.empty(), std::format("environment checked ({} problems here)", environment.mProblems.size()));
  });

  return Testing::Finish();
}
