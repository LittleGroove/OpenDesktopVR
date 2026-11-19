// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Updates end to end (ADR-045): the real setup program installs the real
// host in a private folder and registry key (a test instance); the host
// asks a fake GitHub on 127.0.0.1, whose "release" is that same setup
// program. The host is told it's 0.6.0, so 0.7.0 is newer.
// - A pre-release isn't offered without the beta.
// - A download that doesn't match its checksum is deleted, not installed.
// - Run as administrator (faked), the host warns and runs, and doesn't check:
//   the setup program would refuse to install (Q9).
// - With the beta, the pre-release is downloaded (through a redirect, as
//   GitHub's are), checked, and installed by itself while no game runs and
//   the settings window is closed: the setup program closes the host, which
//   saves and quits, and starts the new one, which says it was updated.
//
//   update-host-test <OpenDesktopVR-Setup.exe>

#include "FakeHttpServer.hpp"
#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Installer.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/Payload.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path gData;

int Run(const fs::path& exe, const std::wstring& arguments) {
  std::wstring line = std::format(L"\"{}\" {}", exe.wstring(), arguments);
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
    return -1;
  }
  DWORD code = static_cast<DWORD>(-1);
  if (WaitForSingleObject(info.hProcess, 120'000) == WAIT_OBJECT_0) {
    GetExitCodeProcess(info.hProcess, &code);
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

std::string HostLog() {
  return FileUtil::ReadAll(gData / L"logs" / L"host.log").value_or("");
}

// The host log, from `from` bytes on, shows `text` within `limit`
bool LogShows(std::size_t from, std::string_view text, std::chrono::seconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  do {
    const auto log = HostLog();
    if (log.size() > from && log.find(text, from) != std::string::npos) {
      return true;
    }
    std::this_thread::sleep_for(200ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

std::string ReleaseList(const std::string& version, bool prerelease, std::uint64_t size, const std::string& sha256,
  const std::string& url) {
  return std::format(R"([{{"tag_name": "v{0}", "draft": false, "prerelease": {1},
    "html_url": "https://github.com/LittleGroove/OpenDesktopVR/releases/tag/v{0}",
    "assets": [{{"name": "OpenDesktopVR-Setup-{0}.exe", "size": {2}, "digest": "sha256:{3}",
                "browser_download_url": "{4}"}}]}}])",
    version, prerelease ? "true" : "false", size, sha256, url);
}

void SetBeta(bool beta) {
  const auto path = gData / L"settings.json";
  auto settings = JsonUtil::Parse(FileUtil::ReadAll(path).value_or("{}")).value_or(JsonUtil::Json::object());
  if (!settings.is_object()) {
    settings = JsonUtil::Json::object();
  }
  settings["updates"] = {{"check", true}, {"installItself", true}, {"beta", beta}};
  FileUtil::WriteAtomic(path, JsonUtil::Dump(settings));
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::printf("usage: update-host-test <OpenDesktopVR-Setup.exe>\n");
    return 2;
  }
  const fs::path setup = fs::absolute(argv[1]);
  const auto setupBytes = FileUtil::ReadAll(setup).value_or("");
  const auto version = Install::SetupProgramVersion(setup).value_or("");
  if (setupBytes.empty() || version.empty()) {
    std::printf("the setup program can't be read\n");
    return 2;
  }
  TempFolder temp;
  const fs::path root {temp.Path()};
  gData = root / L"data";
  const auto instance = std::format(L"update{}", GetCurrentProcessId());

  // The fake GitHub: the list is set per scenario; the download redirects,
  // as GitHub's do; after one install, the list is empty (no loop)
  std::mutex mutex;
  std::string list = "[]";
  int listAsked = 0;
  FakeHttpServer server {[&](const std::string& path) -> FakeHttpServer::Response {
    std::lock_guard lock(mutex);
    if (path.starts_with("/repos/LittleGroove/OpenDesktopVR/releases")) {
      ++listAsked;
      return {200, list};
    }
    if (path.starts_with("/download/")) {
      return {302, "", "/blob/setup.exe"};
    }
    if (path == "/blob/setup.exe") {
      list = "[]";
      return {200, setupBytes};
    }
    return {404, "missing"};
  }};
  const auto assetUrl = server.Url(std::format("/download/v{0}/OpenDesktopVR-Setup-{0}.exe", version));
  const auto setList = [&](std::string text) {
    std::lock_guard lock(mutex);
    list = std::move(text);
  };
  const auto timesListAsked = [&] {
    std::lock_guard lock(mutex);
    return listAsked;
  };

  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", instance.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTALL_ROOT", root.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", gData.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_ALLOW_WARP", L"1");
  SetEnvironmentVariableW(L"OPENDESKTOPVR_UPDATE_URL",
    StringUtil::Widen(server.Url("/repos/LittleGroove/OpenDesktopVR/releases?per_page=30")).c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_FAKE_VERSION", L"0.6.0");
  SetEnvironmentVariableW(L"OPENDESKTOPVR_UPDATE_DELAY_MS", L"500");
  const auto layout = Install::DefaultLayout();
  const auto host = layout.mInstallFolder / Install::HostFileName;
  const auto rightHash = Install::Sha256Hex(setupBytes);

  Scenario("Installed by the real setup program, in a private folder", [&] {
    const int code = Run(setup, L"--silent --no-start");
    Check(code == 0 && FileUtil::Exists(host), std::format("installed {} (exit code {})", version, code));
  });

  Scenario("A pre-release isn't offered without the beta", [&] {
    setList(ReleaseList(version, true, setupBytes.size(), rightHash, assetUrl));
    const auto from = HostLog().size();
    auto running = StartHost(host);
    Check(LogShows(from, "Updates: Up to date: 0.6.0", 30s), "it checked: up to date, on the releases only");
    Check(server.Requests("/blob/setup.exe") == 0, "nothing was downloaded");
    Check(Install::CloseHost(host, 15s) == Install::CloseResult::Closed, "closed");
    CloseHandle(running.hThread);
    CloseHandle(running.hProcess);
  });

  Scenario("A download that doesn't match its checksum is deleted, not installed", [&] {
    setList(ReleaseList(version, false, setupBytes.size(), std::string(64, '0'), assetUrl));
    const auto from = HostLog().size();
    auto running = StartHost(host);
    Check(LogShows(from, "didn't check out (its checksum), so it was deleted", 30s), "refused: the checksum didn't match");
    bool leftover = false;
    std::error_code error;
    for (const auto& entry: fs::directory_iterator(gData / L"updates", error)) {
      leftover = true;
      (void)entry;
    }
    Check(!leftover, "nothing left in the downloads folder");
    Check(WaitForSingleObject(running.hProcess, 0) == WAIT_TIMEOUT, "the host still runs: nothing was installed");
    Check(Install::CloseHost(host, 15s) == Install::CloseResult::Closed, "closed");
    CloseHandle(running.hThread);
    CloseHandle(running.hProcess);
  });

  Scenario("As administrator (faked): it warns and runs, and doesn't check, since setup would refuse (Q9)", [&] {
    SetBeta(true);
    setList(ReleaseList(version, true, setupBytes.size(), rightHash, assetUrl));
    const int asked = timesListAsked();
    const int downloads = server.Requests("/blob/setup.exe");// the checksum scenario downloaded once
    const auto from = HostLog().size();
    SetEnvironmentVariableW(L"OPENDESKTOPVR_FAKE_ADMINISTRATOR", L"1");
    auto running = StartHost(host);
    SetEnvironmentVariableW(L"OPENDESKTOPVR_FAKE_ADMINISTRATOR", nullptr);
    Check(LogShows(from, "Running as administrator with UAC on", 30s), "it warns in its log");
    Check(LogShows(from, "Tray: the administrator warning isn't shown in a test instance", 30s),
      "the tray warning's turn came (a test instance only logs it)");
    Check(LogShows(from, "Updates: off while running as administrator", 30s), "updates are off");
    std::this_thread::sleep_for(3s);// the check delay is 500 ms here
    Check(timesListAsked() == asked && server.Requests("/blob/setup.exe") == downloads,
      std::format("GitHub wasn't asked ({} times); nothing downloaded", timesListAsked() - asked));
    Check(WaitForSingleObject(running.hProcess, 0) == WAIT_TIMEOUT, "it still runs: a warning, not a block");
    Check(Install::CloseHost(host, 15s) == Install::CloseResult::Closed, "closed");
    CloseHandle(running.hThread);
    CloseHandle(running.hProcess);
  });

  Scenario("With the beta: downloaded, checked and installed by itself; the new host says it was updated", [&] {
    SetBeta(true);
    setList(ReleaseList(version, true, setupBytes.size(), rightHash, assetUrl));
    const auto from = HostLog().size();
    auto running = StartHost(host);
    Check(LogShows(from, std::format("Updates: {} is downloaded and checked", version), 30s),
      "the pre-release was downloaded (through a redirect) and checked");
    Check(LogShows(from, std::format("Updates: installing {} (by itself", version), 15s),
      "it installs by itself: no game, the settings window closed");
    const bool closed = WaitForSingleObject(running.hProcess, 60'000) == WAIT_OBJECT_0;
    DWORD exitCode = 1;
    GetExitCodeProcess(running.hProcess, &exitCode);
    Check(closed && exitCode == 0, std::format("the setup program closed it, and it quit normally (exit code {})", exitCode));
    CloseHandle(running.hThread);
    CloseHandle(running.hProcess);
    Check(LogShows(from, "Closing: the setup program asked (the Restart Manager)", 5s), "it saved and closed when asked");
    Check(LogShows(from, std::format("Updates: started after an update from {} to 0.6.0", version), 60s),
      "the new host started, saying it was updated");
    Check(Install::CloseHost(host, 15s) == Install::CloseResult::Closed, "the new host closed");
    const auto saved = JsonUtil::Parse(FileUtil::ReadAll(gData / L"settings.json").value_or(""));
    Check(saved && JsonUtil::GetOr<bool>(JsonUtil::GetOr<JsonUtil::Json>(*saved, "updates", {}), "beta", false),
      "the beta setting kept");
  });

  Run(setup, L"--uninstall --silent --remove-data");
  RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\OpenDesktopVR-Tests\\" + instance).c_str());
  return Finish();
}
