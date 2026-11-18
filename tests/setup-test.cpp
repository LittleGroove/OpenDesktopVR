// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The real OpenDesktopVR-Setup.exe (M7a, ADR-043), silent, as a test
// instance: a private install folder, data folder and registry key.
// - The setup program without its files refuses, and installs nothing.
// - A first install: the files, the setup program's copy, the manifest, the
//   Start menu shortcut, the Apps and Features entry, the layer registered
//   (in the test key), Store apps' read access (CMP-4). It runs without
//   asking for elevation, though its name says "Setup".
// - An update while the host runs: the Restart Manager closes it (it saves
//   and logs why), the files are replaced, the new host starts.
// - An update while a game holds the layer: set aside, the game named in
//   the log (CMP-5), cleaned once free.
// - A newer version installed: refused, unless --allow-older.
// - Uninstalling from the installed copy, as Apps and Features does: a temp
//   copy takes over; everything goes but the settings.
// - --remove-data: the settings go too.
// - Over a test build from install.cmd: its helper scripts removed.
//
//   setup-test <OpenDesktopVR-Setup.exe> <odvr-setup-stub.exe>

#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Installer.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path gSetup;
fs::path gStub;
std::wstring gWindowClass;
fs::path gData;

int Run(const fs::path& exe, const std::wstring& arguments) {
  std::wstring line = std::format(L"\"{}\" {}", exe.wstring(), arguments);
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
    return -static_cast<int>(GetLastError());
  }
  DWORD code = static_cast<DWORD>(-1);
  if (WaitForSingleObject(info.hProcess, 120'000) == WAIT_OBJECT_0) {
    GetExitCodeProcess(info.hProcess, &code);
  } else {
    TerminateProcess(info.hProcess, 99);
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return static_cast<int>(code);
}

PROCESS_INFORMATION StartHost(const fs::path& exe) {
  std::wstring line = std::format(L"\"{}\" --static-panels", exe.wstring());
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info);
  return info;
}

template <class Condition>
bool WaitFor(std::chrono::milliseconds limit, Condition&& condition) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(100ms);
  }
  return condition();
}

bool HostWindowUp() {
  return FindWindowExW(HWND_MESSAGE, nullptr, gWindowClass.c_str(), nullptr) != nullptr;
}

std::string Log(const wchar_t* name) {
  return FileUtil::ReadAll(gData / L"logs" / name).value_or("");
}

// The log's lines from its last run
std::string LastRun(const wchar_t* name, std::string_view started) {
  const auto text = Log(name);
  const auto at = text.rfind(started);
  return at == std::string::npos ? std::string {} : text.substr(at);
}

std::string LastSetup() {
  return LastRun(L"setup.log", "Open Desktop VR setup started");
}

bool LayerRegistered(const Install::Layout& layout, const fs::path& manifest) {
  DWORD value = 1;
  DWORD size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, layout.mLayerKey.c_str(), manifest.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size)
      == ERROR_SUCCESS
    && value == 0;
}

std::wstring KeyText(const Install::Layout& layout, const wchar_t* name) {
  wchar_t buffer[1024] {};
  DWORD size = sizeof(buffer) - sizeof(wchar_t);
  RegGetValueW(HKEY_CURRENT_USER, layout.mUninstallKey.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer, &size);
  return buffer;
}

void WriteText(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream file {path, std::ios::binary | std::ios::trunc};
  file << text;
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::printf("usage: setup-test <OpenDesktopVR-Setup.exe> <odvr-setup-stub.exe>\n");
    return 2;
  }
  gSetup = fs::absolute(argv[1]);
  gStub = fs::absolute(argv[2]);
  TempFolder temp;
  const fs::path root {temp.Path()};
  gData = root / L"data";
  const auto instance = std::format(L"setup{}", GetCurrentProcessId());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", instance.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTALL_ROOT", root.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", gData.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_ALLOW_WARP", L"1");
  gWindowClass = SharedObjectName(L"Automation", SharedAbi::AbiVersion);
  gWindowClass.erase(0, gWindowClass.find(L'\\') + 1);
  const auto layout = Install::DefaultLayout();
  const auto& folder = layout.mInstallFolder;
  const auto host = folder / Install::HostFileName;
  const auto testStart = fs::file_time_type::clock::now();

  Scenario("The setup program without its files refuses, and installs nothing", [&] {
    const int code = Run(gStub, L"--silent");
    Check(code == 1, std::format("exit code 1 ({})", code));
    Check(!fs::exists(folder), "no install folder");
    Check(LastSetup().find("carries no files") != std::string::npos, "its log says why");
  });

  Scenario("A first install: files, shortcut, Apps and Features, the layer, Store apps' access", [&] {
    const int code = Run(gSetup, L"--silent --no-start");
    Check(code == 0, std::format("done, without asking for elevation (exit code {})", code));
    int present = 0;
    for (const std::wstring_view file: {Install::HostFileName, Product::LayerDllName, Product::LayerManifestName,
           Product::AppHelperDllName, std::wstring_view {L"odvr-layer.exe"}, std::wstring_view {L"tools\\odvr-mirror.exe"},
           std::wstring_view {L"BUILD-INFO.txt"}, std::wstring_view {L"LICENSE"}, Install::SetupFileName,
           Install::ManifestFileName}) {
      present += FileUtil::Exists(folder / file) ? 1 : 0;
    }
    Check(present == 10, std::format("the program's 10 files, its setup copy and its manifest ({})", present));
    Check(!FileUtil::Exists(folder / L"install.cmd") && !FileUtil::Exists(folder / L"tools" / L"openxr-simulator.exe"),
      "no test-build scripts or test tools");
    Check(fs::exists(layout.mShortcut), "the Start menu shortcut");
    const auto installed = Install::FindInstalled(layout);
    Check(installed && installed->mVersion == Product::Version && installed->mFolder == folder,
      std::format("Apps and Features lists version {}", Product::Version));
    Check(KeyText(layout, L"UninstallString").find(L"OpenDesktopVR-Setup.exe\" --uninstall") != std::wstring::npos,
      "its uninstall runs the setup program's copy");
    Check(LayerRegistered(layout, folder / Product::LayerManifestName), "the layer registered, enabled (the test key)");
    Check(Install::HasAppPackagesRead(folder), "Store apps may read the folder (CMP-4)");
    Check(LastSetup().find("Setup finished: exit code 0") != std::string::npos, "its log says it finished");
  });

  Scenario("An update while it runs: the Restart Manager closes it, it saves, the new one starts", [&] {
    auto running = StartHost(host);
    if (!Check(running.hProcess && WaitFor(30s, HostWindowUp), "the installed host is running")) {
      return;
    }
    std::this_thread::sleep_for(1s);
    const int code = Run(gSetup, L"--silent");
    Check(code == 0, std::format("updated (exit code {})", code));
    DWORD exitCode = 1;
    const bool closed = WaitForSingleObject(running.hProcess, 5'000) == WAIT_OBJECT_0;
    GetExitCodeProcess(running.hProcess, &exitCode);
    Check(closed && exitCode == 0, std::format("the running host was closed and quit normally (exit code {})", exitCode));
    CloseHandle(running.hThread);
    CloseHandle(running.hProcess);
    Check(Log(L"host.log").find("Closing: the setup program asked (the Restart Manager)") != std::string::npos,
      "its log says the setup program asked it to close");
    Check(LastSetup().find("was running: closed") != std::string::npos, "the setup log says it closed it");
    Check(WaitFor(30s, HostWindowUp), "the new host started");
    std::this_thread::sleep_for(1s);
    Check(Install::CloseHost(host, 15s) == Install::CloseResult::Closed, "and closes the same way (as the next setup would)");
    Check(!HostWindowUp(), "none left running");
  });

  Scenario("An update while a game holds the layer: set aside, the game named (CMP-5), cleaned once free", [&] {
    const HMODULE held = LoadLibraryExW((folder / Product::LayerDllName).c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!Check(held != nullptr, "the layer loaded, as a game holds it")) {
      return;
    }
    const int code = Run(gSetup, L"--silent --no-start");
    Check(code == 0, std::format("updated (exit code {})", code));
    const auto log = LastSetup();
    Check(log.find("1 in use set aside") != std::string::npos, "the layer was set aside");
    Check(log.find("still on the old files until restarted: games 'setup-test") != std::string::npos,
      "the setup log names the program still holding it");
    FreeLibrary(held);
    Check(Install::CleanAside(folder) == 0, "once free, the old one is deleted (as the host's next start does)");
  });

  Scenario("A newer version installed: refused, unless --allow-older", [&] {
    const std::wstring newer = L"99.0.0";
    RegSetKeyValueW(HKEY_CURRENT_USER, layout.mUninstallKey.c_str(), L"DisplayVersion", REG_SZ, newer.c_str(),
      static_cast<DWORD>((newer.size() + 1) * sizeof(wchar_t)));
    const int refused = Run(gSetup, L"--silent --no-start");
    Check(refused == 4, std::format("refused (exit code {})", refused));
    const int replaced = Run(gSetup, L"--silent --no-start --allow-older");
    const auto installed = Install::FindInstalled(layout);
    Check(replaced == 0 && installed && installed->mVersion == Product::Version,
      std::format("replaced with --allow-older (exit code {})", replaced));
  });

  Scenario("Uninstall from the installed copy, as Apps and Features runs it: all goes but the settings", [&] {
    const bool hadSettings = FileUtil::Exists(gData / L"settings.json");
    const int code = Run(folder / Install::SetupFileName, L"--uninstall --silent");
    Check(code == 0, std::format("handed over to a temp copy (exit code {})", code));
    const bool gone = WaitFor(60s, [&] { return !fs::exists(folder) && !Install::FindInstalled(layout); });
    Check(gone, "the install folder and the Apps and Features entry are gone");
    Check(!fs::exists(layout.mShortcut), "the shortcut is gone");
    Check(!LayerRegistered(layout, folder / Product::LayerManifestName), "the layer is unregistered");
    Check(hadSettings && FileUtil::Exists(gData / L"settings.json"), "the settings are kept");
    Check(Log(L"setup.log").find("uninstalling from a copy in the temp folder") != std::string::npos,
      "the log says a temp copy did it");
    // The temp copy can't delete itself; the next setup does, and so does this test
    std::error_code error;
    const auto tempFolder = fs::temp_directory_path(error);
    WaitFor(10s, [&] {
      bool left = false;
      for (const auto& entry: fs::directory_iterator(tempFolder, error)) {
        const auto name = entry.path().filename().native();
        if (name.starts_with(L"OpenDesktopVR-Uninstall-") && entry.last_write_time(error) >= testStart) {
          left = !DeleteFileW(entry.path().c_str()) || left;
        }
      }
      return !left;
    });
  });

  Scenario("Uninstall while a game holds the layer: the rest goes, the holder is named", [&] {
    Check(Run(gSetup, L"--silent --no-start") == 0, "installed");
    const HMODULE held = LoadLibraryExW((folder / Product::LayerDllName).c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    const int code = Run(gSetup, L"--uninstall --silent");
    Check(held && code == 0, std::format("uninstalled (exit code {})", code));
    const auto log = LastSetup();
    Check(fs::exists(folder) && !FileUtil::Exists(folder / Install::HostFileName) && !Install::FindInstalled(layout),
      "everything but the held file is gone");
    Check(log.find("still holding files: 'setup-test") != std::string::npos, "the log names the program holding it");
    FreeLibrary(held);
    Install::RemoveFiles(folder);
    Check(!fs::exists(folder), "once free, the folder can go");
  });

  Scenario("Install, then uninstall with --remove-data: the settings go too", [&] {
    const int installed = Run(gSetup, L"--silent --no-start");
    const int removed = Run(gSetup, L"--uninstall --silent --remove-data");
    Check(installed == 0 && removed == 0, std::format("installed and uninstalled ({}, {})", installed, removed));
    Check(!fs::exists(folder) && !fs::exists(gData), "the install folder and the data folder are gone");
  });

  Scenario("Over a test build from install.cmd: updated, its scripts and test tools removed", [&] {
    for (const auto* file: {L"install.cmd", L"uninstall.cmd", L"command-prompt.cmd", L"tools\\openxr-simulator.exe",
           L"OpenDesktopVR.exe"}) {
      WriteText(folder / file, "old");
    }
    const auto before = Install::FindInstalled(layout);
    Check(before && before->mVersion.empty(), "found as a test build");
    const int code = Run(gSetup, L"--silent --no-start");
    Check(code == 0 && LastSetup().find("installed: a test build") != std::string::npos,
      std::format("updated it (exit code {})", code));
    Check(!FileUtil::Exists(folder / L"install.cmd") && !FileUtil::Exists(folder / L"uninstall.cmd")
        && !FileUtil::Exists(folder / L"command-prompt.cmd") && !FileUtil::Exists(folder / L"tools" / L"openxr-simulator.exe"),
      "its scripts and the test tool removed");
    Check(FileUtil::Size(host).value_or(0) > 100'000, "the real program in place");
    Run(gSetup, L"--uninstall --silent --remove-data");
  });

  if (HostWindowUp()) {
    Install::CloseHost(host, 15s);
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\OpenDesktopVR-Tests\\" + instance).c_str());
  return Finish();
}
