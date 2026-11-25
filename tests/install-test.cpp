// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The installer's parts (M7a, ADR-043), in a private folder and registry
// key:
// - versions: parsing, order (betas before their release), nonsense refused;
// - the payload: packed and unpacked, every damage refused (TH-9), unsafe
//   paths refused both ways, 3,000 random corruptions never accepted wrong;
// - the files: written, an old file removed by the next install, a test
//   build's leftovers removed, a loaded DLL set aside and replaced (CMP-5),
//   set-aside files cleaned once free;
// - an uninstall removes only ours: never a path from a tampered list;
// - the Start menu shortcut, the Apps and Features entry, Store apps' read
//   access (CMP-4), and the Restart Manager finding a file's user;
// - how the setup program starts the new host after an update, and how the
//   host reads it (UP-8).
//
//   install-test

#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Installer.hpp>
#include <OpenDesktopVR/Payload.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/Version.hpp>

#include <Windows.h>

#include <ShlObj.h>
#include <wrl/client.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <random>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;
namespace fs = std::filesystem;

namespace {

std::string Random(std::mt19937& random, std::size_t size) {
  std::string data(size, '\0');
  for (auto& c: data) {
    c = static_cast<char>(random() & 0xFF);
  }
  return data;
}

void WriteText(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream file {path, std::ios::binary | std::ios::trunc};
  file << text;
}

Install::Payload SamplePayload(std::mt19937& random) {
  Install::Payload payload;
  payload.mVersion = "0.7.0";
  payload.mBuild = "commit test on m7";
  payload.mFiles = {
    {"OpenDesktopVR.exe", Random(random, 300'000)},
    {"BUILD-INFO.txt", "Open Desktop VR 0.7.0\r\n"},
    {"empty.txt", ""},
    {"tools/odvr-mirror.exe", std::string(1 << 20, 'x')},
    {"licenses/Apache-2.0.txt", "Apache License"},
  };
  return payload;
}

std::wstring ShortcutTarget(const fs::path& shortcut) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  Microsoft::WRL::ComPtr<IPersistFile> file;
  wchar_t target[MAX_PATH] {};
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))
      && SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Load(shortcut.c_str(), STGM_READ))) {
    link->GetPath(target, MAX_PATH, nullptr, 0);
  }
  return target;
}

}// namespace

int wmain() {
  std::mt19937 random {20261118};
  TempFolder temp;
  const fs::path root {temp.Path()};
  const auto instance = std::format(L"install{}", GetCurrentProcessId());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", instance.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTALL_ROOT", root.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", (root / L"data").c_str());

  Scenario("Versions: parsed, ordered (a beta before its release), nonsense refused", [&] {
    const auto v = [](std::string_view text) { return Install::ParseVersion(text); };
    Check(v("0.7.0") && v("0.7.0")->ToString() == "0.7.0", "0.7.0");
    Check(v("v1.2.3") && v("v1.2.3")->ToString() == "1.2.3", "a tag's v");
    Check(v("0.7.1-beta.2") && v("0.7.1-beta.2")->IsBeta() && v("0.7.1-beta.2")->mBeta == 2, "0.7.1-beta.2");
    Check(v(Product::Version).has_value(), std::format("this build's own version ({})", Product::Version));
    Check(*v("0.7.0-beta.1") < *v("0.7.0-beta.2") && *v("0.7.0-beta.2") < *v("0.7.0") && *v("0.7.0") < *v("0.7.1-beta.1")
        && *v("0.7.1-beta.1") < *v("0.7.1") && *v("0.7.1") < *v("0.10.0") && *v("0.10.0") < *v("1.0.0"),
      "0.7.0-beta.1 < 0.7.0-beta.2 < 0.7.0 < 0.7.1-beta.1 < 0.7.1 < 0.10.0 < 1.0.0");
    Check(*v("0.7.0") == *v("v0.7.0"), "equal with or without v");
    int refused = 0;
    for (const char* bad: {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.3-beta", "1.2.3-beta.0", "1.2.3-rc.1",
           "1.2.3-beta.1x", "100000.0.0", "1.2.3 ", " 1.2.3", "a.b.c", "1..3", "-1.2.3", "1.2.3-beta.01"}) {
      refused += v(bad) ? 0 : 1;
    }
    Check(refused == 16, std::format("16 malformed versions refused ({})", refused));
    // Random strings of a version's characters, so some parse
    constexpr std::string_view alphabet = "0123456789.-betav";
    int accepted = 0;
    int roundTrips = 0;
    for (int i = 0; i < 20'000; ++i) {
      std::string text;
      const auto length = random() % 16;
      for (unsigned c = 0; c < length; ++c) {
        text.push_back(alphabet[random() % alphabet.size()]);
      }
      if (const auto parsed = v(text)) {
        ++accepted;
        roundTrips += v(parsed->ToString()) == parsed ? 1 : 0;
      }
    }
    Check(accepted > 0 && roundTrips == accepted,
      std::format("20,000 random strings: {} parsed, and each one's text parses back the same", accepted));
  });

  Scenario("SHA-256 (Windows' CNG)", [&] {
    Check(Install::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "the FIPS 180-2 test vector for \"abc\"");
  });

  Scenario("Paths that stay inside the folder; unsafe ones refused", [&] {
    int safe = 0;
    for (const char* good: {"OpenDesktopVR.exe", "tools/odvr-mirror.exe", "licenses/Apache-2.0.txt", "a b/c.d"}) {
      safe += Install::IsSafeRelativePath(good) ? 1 : 0;
    }
    Check(safe == 4, "four ordinary paths are safe");
    int unsafe = 0;
    for (const char* bad: {"", "../x", "a/../b", "/x", "x/", "C:/x", "C:x", "a\\b", "a//b", "./a", "a/.", "CON",
           "tools/nul.txt", "com1", "LPT9.dll", "a?b", "a*b", "a|b", "a\"b", "a<b", "a>b", "a.", "a ", "a\tb"}) {
      unsafe += Install::IsSafeRelativePath(bad) ? 0 : 1;
    }
    Check(unsafe == 24, std::format("24 unsafe paths refused ({})", unsafe));
    Check(!Install::IsSafeRelativePath(std::string(Install::MaxPathLength + 1, 'a')), "an over-long path refused");
  });

  Scenario("The payload: packed, unpacked, the same files", [&] {
    const auto payload = SamplePayload(random);
    std::string error;
    const auto bytes = Install::Pack(payload, &error);
    if (!Check(bytes.has_value(), "packed" + error)) {
      return;
    }
    Check(bytes->size() < 600'000, std::format("compressed: {} KB for 1.3 MB", bytes->size() / 1024));
    const auto back = Install::Unpack(*bytes, &error);
    if (!Check(back.has_value(), "unpacked" + error)) {
      return;
    }
    bool same = back->mFiles.size() == payload.mFiles.size() && back->mVersion == "0.7.0" && back->mBuild == payload.mBuild;
    for (std::size_t i = 0; same && i < payload.mFiles.size(); ++i) {
      same = back->mFiles[i].mPath == payload.mFiles[i].mPath && back->mFiles[i].mData == payload.mFiles[i].mData;
    }
    Check(same, "the same version, build and files, an empty one included");
  });

  Scenario("The payload: unsafe input refused when packing", [&] {
    for (const char* path: {"../evil.dll", "C:/Windows/evil.dll", "tools\\x.exe"}) {
      Install::Payload bad;
      bad.mVersion = "0.7.0";
      bad.mFiles = {{path, "x"}};
      Check(!Install::Pack(bad).has_value(), std::format("'{}' refused", path));
    }
    Install::Payload twice;
    twice.mVersion = "0.7.0";
    twice.mFiles = {{"a.txt", "1"}, {"A.TXT", "2"}};
    Check(!Install::Pack(twice).has_value(), "the same file twice (ignoring case) refused");
    Install::Payload noVersion;
    noVersion.mVersion = "seven";
    noVersion.mFiles = {{"a.txt", "1"}};
    Check(!Install::Pack(noVersion).has_value(), "a version that isn't one refused");
  });

  Scenario("The payload: damage refused when unpacking (TH-9)", [&] {
    const auto payload = SamplePayload(random);
    const auto bytes = *Install::Pack(payload);
    // Unpacked first, then reported (a call's arguments may be made in any order)
    const auto refuse = [](std::string_view damaged, std::string_view what) {
      std::string why;
      const bool refused = !Install::Unpack(damaged, &why);
      Check(refused && !why.empty(), std::format("{}: {}", what, why));
    };
    refuse(bytes.substr(0, bytes.size() - 1), "one byte short");
    refuse(bytes + "x", "one byte extra");
    refuse("ODVRPAK1", "no header");
    refuse("", "nothing");
    auto flipped = bytes;
    flipped[flipped.size() - 100] ^= 0x40;
    refuse(flipped, "a flipped byte in a file");
    auto lie = bytes;
    lie[8] = static_cast<char>(0xFF);
    lie[9] = static_cast<char>(0xFF);
    lie[10] = static_cast<char>(0xFF);
    refuse(lie, "a header size past the end");
    // A header naming a path outside the folder, with a correct hash
    const std::string data = "evil";
    const auto header = std::format(R"({{"version":"0.7.0","build":"","files":[{{"path":"../evil.dll","size":4,"packed":0,"sha256":"{}"}}]}})",
      Install::Sha256Hex(data));
    std::string crafted = "ODVRPAK1";
    for (int shift = 0; shift < 32; shift += 8) {
      crafted.push_back(static_cast<char>((header.size() >> shift) & 0xFF));
    }
    crafted += header;
    crafted += data;
    refuse(crafted, "a path outside the folder in the header");

    int accepted = 0;
    int wrong = 0;
    for (int i = 0; i < 3'000; ++i) {
      auto damaged = bytes;
      const int changes = 1 + static_cast<int>(random() % 4);
      for (int c = 0; c < changes; ++c) {
        damaged[random() % damaged.size()] = static_cast<char>(random() & 0xFF);
      }
      if (random() % 5 == 0) {
        damaged.resize(random() % damaged.size());
      }
      if (const auto back = Install::Unpack(damaged)) {
        ++accepted;
        for (std::size_t f = 0; f < back->mFiles.size() && f < payload.mFiles.size(); ++f) {
          wrong += back->mFiles[f].mData != payload.mFiles[f].mData ? 1 : 0;
        }
      }
    }
    Check(wrong == 0, std::format("3,000 random corruptions: none accepted with different files ({} harmless ones accepted)", accepted));
  });

  const fs::path folder = root / L"Programs" / L"OpenDesktopVR";
  Scenario("Files: written and listed; the next install removes what it no longer has", [&] {
    auto payload = SamplePayload(random);
    const std::string extra[] {"OpenDesktopVR-Setup.exe"};
    std::size_t progressCalls = 0;
    auto report = Install::WriteFiles(payload, folder, extra, [&](std::size_t, std::size_t) { ++progressCalls; });
    Check(report.mOk && report.mWritten == 5 && progressCalls == 5, std::format("5 files written ({}) {}", report.mWritten, report.mError));
    Check(FileUtil::ReadAll(folder / L"tools" / L"odvr-mirror.exe") == payload.mFiles[3].mData, "a file in a subfolder, as packed");
    const auto manifest = FileUtil::ReadAll(folder / Install::ManifestFileName).value_or("");
    Check(manifest.find("OpenDesktopVR-Setup.exe") != std::string::npos && manifest.find("tools/odvr-mirror.exe") != std::string::npos,
      "the manifest lists them, and the setup program's copy");
    payload.mFiles.erase(payload.mFiles.begin() + 2);// empty.txt
    report = Install::WriteFiles(payload, folder, extra);
    Check(report.mOk && report.mRemoved == 1 && !FileUtil::Exists(folder / L"empty.txt"), "the next install removed the file it no longer has");
    Check(Install::CleanAside(folder) == 0, "nothing set aside");
  });

  Scenario("Files: one that can't be written leaves the install as it was", [&] {
    const fs::path place = root / L"half";
    Install::Payload first;
    first.mVersion = "0.7.0";
    first.mFiles = {{"a.txt", "old"}};
    Check(Install::WriteFiles(first, place).mOk, "installed");
    // "blocker" is a file, so nothing can be written inside it
    WriteText(place / L"blocker", "a file, not a folder");
    Install::Payload second;
    second.mVersion = "0.7.1";
    second.mFiles = {{"a.txt", "new"}, {"blocker/inner.txt", "x"}};
    const auto report = Install::WriteFiles(second, place);
    Check(!report.mOk && report.mError.find("nothing was changed") != std::string::npos, "refused: " + report.mError);
    Check(FileUtil::ReadAll(place / L"a.txt") == "old", "the file written before it is still the old one");
    bool leftover = false;
    for (const auto& entry: fs::recursive_directory_iterator(place)) {
      leftover = leftover || entry.path().filename().native().ends_with(Install::NewSuffix);
    }
    Check(!leftover, "no half-written files left");
  });

  Scenario("Files: a test build's leftovers removed (no manifest yet)", [&] {
    const fs::path old = root / L"old-install";
    WriteText(old / L"install.cmd", "@echo off");
    WriteText(old / L"tools" / L"click-target.exe", "MZ");
    WriteText(old / L"my-notes.txt", "mine");
    const auto report = Install::WriteFiles(SamplePayload(random), old);
    Check(report.mOk && report.mRemoved == 2, std::format("install.cmd and the test tool removed ({})", report.mRemoved));
    Check(FileUtil::Exists(old / L"my-notes.txt"), "a file that isn't ours left alone");
  });

  Scenario("Files: a loaded DLL is set aside and replaced (CMP-5), then cleaned once free", [&] {
    const auto exeFolder = ProcessUtil::ExecutablePath().parent_path();
    const auto first = FileUtil::ReadAll(exeFolder / Product::AppHelperDllName);
    const auto second = FileUtil::ReadAll(exeFolder / Product::LayerDllName);
    if (!Check(first && second, "two DLLs to install")) {
      return;
    }
    const fs::path dllFolder = root / L"dll-install";
    Install::Payload payload;
    payload.mVersion = "0.7.0";
    payload.mFiles = {{"game-part.dll", *first}};
    Check(Install::WriteFiles(payload, dllFolder).mOk, "installed");
    // As a game holds the layer: mapped as an image
    const HMODULE held = LoadLibraryExW((dllFolder / L"game-part.dll").c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!Check(held != nullptr, "loaded, as a game would")) {
      return;
    }
    const fs::path heldFile[] {dllFolder / L"game-part.dll"};
    const auto users = Install::FindFileUsers(heldFile);
    Check(std::any_of(users.begin(), users.end(), [](const auto& user) { return user.mProcessID == GetCurrentProcessId(); }),
      std::format("the Restart Manager names this process as its user ({} users)", users.size()));
    payload.mFiles[0].mData = *second;
    const auto report = Install::WriteFiles(payload, dllFolder);
    Check(report.mOk && report.mSetAside == 1, std::format("updated while loaded: set aside ({}) {}", report.mSetAside, report.mError));
    Check(FileUtil::ReadAll(dllFolder / L"game-part.dll") == *second, "the new one is in its place");
    Check(Install::CleanAside(dllFolder) == 1, "the old one can't be deleted while loaded");
    FreeLibrary(held);
    Check(Install::CleanAside(dllFolder) == 0, "once free, it's deleted");
    bool leftover = false;
    for (const auto& entry: fs::directory_iterator(dllFolder)) {
      leftover = leftover || entry.path().filename().native().find(Install::AsideSuffix) != std::wstring::npos;
    }
    Check(!leftover, "no set-aside file left");
  });

  Scenario("An uninstall removes only ours: a tampered list can't reach outside", [&] {
    const fs::path outside = root / L"outside.txt";
    WriteText(outside, "not ours");
    const fs::path tampered = root / L"tampered";
    WriteText(tampered / L"OpenDesktopVR.exe", "MZ");
    WriteText(tampered / Install::ManifestFileName,
      std::format(R"({{"version":"0.7.0","files":["OpenDesktopVR.exe","../outside.txt","{}",42]}})",
        StringUtil::Narrow(outside.generic_wstring())));
    const auto report = Install::RemoveFiles(tampered);
    Check(FileUtil::Exists(outside), "the file outside the folder is still there");
    Check(report.mOk && !fs::exists(tampered), std::format("the folder is gone ({} removed)", report.mRemoved));

    const fs::path withMine = root / L"with-mine";
    WriteText(withMine / L"OpenDesktopVR.exe", "MZ");
    WriteText(withMine / L"my-notes.txt", "mine");
    const auto kept = Install::RemoveFiles(withMine);
    Check(!kept.mOk && FileUtil::Exists(withMine / L"my-notes.txt") && !FileUtil::Exists(withMine / L"OpenDesktopVR.exe"),
      "without a manifest: the shipped files go, a stranger's file and so the folder stay");
  });

  Scenario("The Start menu shortcut, the Apps and Features entry, Store apps' access (CMP-4)", [&] {
    const auto layout = Install::DefaultLayout();
    Check(layout.mTest && layout.mInstallFolder == folder && layout.mUninstallKey.find(L"OpenDesktopVR-Tests") != std::wstring::npos
        && layout.mLayerKey.find(L"OpenDesktopVR-Tests") != std::wstring::npos,
      "a test instance's layout is private: a temp folder and a test registry key");
    const auto target = folder / L"OpenDesktopVR.exe";
    Check(Install::WriteShortcut(layout.mShortcut, target, L"test"), "the shortcut written");
    Check(fs::equivalent(ShortcutTarget(layout.mShortcut), target), "it points to the program");
    Check(Install::RemoveShortcut(layout.mShortcut) && !fs::exists(layout.mShortcut), "and goes");

    Check(!Install::FindInstalled(Install::Layout {root / L"none", {}, layout.mUninstallKey + L"-none", {}, {}, true}),
      "nothing installed: none found");
    const Install::UninstallEntry entry {"0.7.0", folder, folder / Install::SetupFileName, 1'500'000};
    Check(Install::WriteUninstallEntry(layout, entry), "the Apps and Features entry written");
    const auto installed = Install::FindInstalled(layout);
    Check(installed && installed->mVersion == "0.7.0" && installed->mFolder == folder, "found again, with its version and folder");
    Check(Install::RemoveUninstallEntry(layout), "removed");
    const auto testBuild = Install::FindInstalled(layout);
    Check(testBuild && testBuild->mVersion.empty(), "without the entry, the program's folder reads as a test build");

    Check(!Install::HasAppPackagesRead(folder), "Store apps can't read the folder at first");
    Check(Install::GrantAppPackagesRead(folder) && Install::HasAppPackagesRead(folder), "after the grant, they can");
  });

  Scenario("After an update: the new host's arguments say from what, and how (UP-8)", [&] {
    using Install::UpdatedBy;
    Check(Install::HostArguments("0.7.0", UpdatedBy::Setup) == L"--updated-from 0.7.0",
      "the user ran the setup program: only the version");
    Check(Install::HostArguments("0.7.0", UpdatedBy::Itself) == L"--updated-from 0.7.0 --updated-by itself",
      "an update by itself");
    Check(Install::HostArguments("0.7.1-beta.1", UpdatedBy::Request)
        == L"--updated-from 0.7.1-beta.1 --updated-by request",
      "an update asked for, from a beta");
    Check(Install::HostArguments("", UpdatedBy::Itself).empty() && Install::HostArguments("not a version", UpdatedBy::Request).empty()
        && Install::HostArguments("0.7.0 --settings", UpdatedBy::Itself).empty(),
      "not an update, or not a version: no arguments at all");
    Check(Install::ParseUpdatedBy(L"itself") == UpdatedBy::Itself && Install::ParseUpdatedBy(L"request") == UpdatedBy::Request,
      "the host reads both");
    Check(Install::ParseUpdatedBy(L"") == UpdatedBy::Setup && Install::ParseUpdatedBy(L"Itself") == UpdatedBy::Setup
        && Install::ParseUpdatedBy(L"request ") == UpdatedBy::Setup,
      "anything else reads as the setup program's own: no notice, no window");
  });

  RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\OpenDesktopVR-Tests\\" + instance).c_str());
  return Finish();
}
