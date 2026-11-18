// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Doc 11's robustness tests with the real host (M6f), in a private instance
// and data folder:
// - unreadable settings and profiles (garbage, truncated, empty): the host
//   starts with defaults, keeps the bad files aside, and does what it's told
//   (ST-4);
// - profile switches, by command, with the settings window open: no crash,
//   the window carrying on with the panels it now has (DU-2);
// - the shared memory's name held by another program, as an elevated game
//   holds it (here, with a DACL that refuses everyone): the host says so and
//   what to do, carries on, and makes the block once it's let go (CMP-8);
// - started on a game's profile with no game running (as an update mid-game
//   leaves it): back to the profile it replaced (test round 23).
//
//   robustness-host-test <OpenDesktopVR.exe>

#include "check.hpp"

#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>

#include <Windows.h>

#include <sddl.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;
namespace fs = std::filesystem;

namespace {

std::wstring gHostExe;
std::wstring gWindowClass;
fs::path gData;

std::wstring Quoted(const std::wstring& word) {
  return word.find_first_of(L" \t") == std::wstring::npos && !word.empty() ? word : L"\"" + word + L"\"";
}

PROCESS_INFORMATION Start(const std::wstring& arguments) {
  auto line = std::format(L"\"{}\" --no-register --warp --static-panels {}", gHostExe, arguments);
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(gHostExe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
    return {};
  }
  return info;
}

// Its exit code once it quits by itself; -1 if it had to be stopped
int WaitExit(PROCESS_INFORMATION& host) {
  DWORD code = static_cast<DWORD>(-1);
  if (host.hProcess && WaitForSingleObject(host.hProcess, 60'000) == WAIT_OBJECT_0) {
    GetExitCodeProcess(host.hProcess, &code);
  } else if (host.hProcess) {
    TerminateProcess(host.hProcess, 1);
  }
  if (host.hProcess) {
    CloseHandle(host.hThread);
    CloseHandle(host.hProcess);
  }
  host = {};
  return static_cast<int>(code);
}

bool WaitForWindow() {
  for (int i = 0; i < 200; ++i) {
    if (FindWindowExW(HWND_MESSAGE, nullptr, gWindowClass.c_str(), nullptr)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

int RunCommand(const std::vector<std::wstring>& words) {
  std::wstring line = Quoted(gHostExe);
  for (const auto& word: words) {
    line += L" " + Quoted(word);
  }
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(gHostExe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
    return -1;
  }
  DWORD code = static_cast<DWORD>(-1);
  if (WaitForSingleObject(info.hProcess, 10'000) == WAIT_OBJECT_0) {
    GetExitCodeProcess(info.hProcess, &code);
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return static_cast<int>(code);
}

std::string ReadText(const fs::path& path) {
  std::ifstream file {path, std::ios::binary};
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream file {path, std::ios::binary | std::ios::trunc};
  file << text;
}

std::string HostLog() {
  return ReadText(gData / "logs" / "host.log");
}

// The host's log has `text` within `seconds` (it's written as it goes)
bool LogShows(const std::string& text, int seconds) {
  for (int i = 0; i < seconds * 10; ++i) {
    if (HostLog().find(text) != std::string::npos) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

int CountFiles(const fs::path& folder, std::wstring_view ending) {
  int count = 0;
  std::error_code ec;
  for (const auto& entry: fs::directory_iterator(folder, ec)) {
    count += entry.path().wstring().ends_with(ending) ? 1 : 0;
  }
  return count;
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::printf("usage: robustness-host-test <host exe>\n");
    return 2;
  }
  gHostExe = argv[1];
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", std::format(L"rob{}", GetCurrentProcessId()).c_str());
  TempFolder data;
  gData = data.Path();
  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", data.Path().c_str());
  gWindowClass = SharedObjectName(L"Automation", SharedAbi::AbiVersion);
  gWindowClass.erase(0, gWindowClass.find(L'\\') + 1);

  Scenario("Unreadable settings and profiles: defaults, the files kept aside, the host working (ST-4)", [&] {
    WriteText(gData / "settings.json", "{ this isn't JSON");
    WriteText(gData / "profiles" / "0123456789abcdef.json", R"({"id": "0123456789abcdef", "name": "Broken", "panels": [)");
    WriteText(gData / "profiles" / "fedcba9876543210.json", "");
    auto host = Start(L"--exit-after 10");
    Check(WaitForWindow(), "the host starts anyway");
    Check(RunCommand({L"toggle", L"all"}) == 0, "and does what it's told");
    Check(WaitExit(host) == 0, "and quits normally");
    Check(fs::exists(gData / "settings.json.unreadable"), "the unreadable settings.json kept aside");
    Check(CountFiles(gData / "profiles", L".unreadable") >= 2, std::format("both unreadable profiles kept aside ({})",
      CountFiles(gData / "profiles", L".unreadable")));
    const auto log = HostLog();
    Check(log.find("unreadable, set aside") != std::string::npos && log.find("'Default' with 2 panels") != std::string::npos,
      "the log says so, and the host made a fresh 'Default' with the test panels");
  });

  Scenario("Profile switches with the settings window open: no crash, the window carrying on (DU-2)", [&] {
    // A second profile: a copy of 'Default' under another ID and name
    fs::path defaultFile;
    for (const auto& entry: fs::directory_iterator(gData / "profiles")) {
      if (entry.path().extension() == ".json") {
        auto json = JsonUtil::Json::parse(ReadText(entry.path()), nullptr, false);
        if (json.is_object() && json.value("name", "") == "Default") {
          json["id"] = "aaaaaaaaaaaaaaaa";
          json["name"] = "Second";
          WriteText(gData / "profiles" / "aaaaaaaaaaaaaaaa.json", json.dump(2));
          defaultFile = entry.path();
        }
      }
    }
    if (!Check(!defaultFile.empty(), "a second profile made from 'Default'")) {
      return;
    }
    auto host = Start(L"--settings --exit-after 14");
    Check(WaitForWindow(), "the host starts, its settings window open");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const std::vector<std::vector<std::wstring>> commands {
      {L"profile", L"next"},
      {L"nudge", L"#1", L"left", L"1"},
      {L"profile", L"previous"},
      {L"toggle", L"all"},
      {L"profile", L"Second"},
      {L"opacity", L"#2", L"-10"},
      {L"profile", L"Default"},
      {L"show", L"all"},
    };
    int done = 0;
    for (const auto& words: commands) {
      done += RunCommand(words) == 0 ? 1 : 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(300));// a frame or two of the window between
    }
    Check(done == static_cast<int>(commands.size()), std::format("{} of {} commands done", done, commands.size()));
    Check(WaitExit(host) == 0, "the host quit normally afterwards");
    const auto log = HostLog();
    Check(log.find("Profile: 'Second' (a command)") != std::string::npos && log.find("Profile: 'Default' (a command)") != std::string::npos,
      "both profiles were switched to");
  });

  Scenario("Another program holds the shared memory's name (an elevated game): said, carried on, recovered (CMP-8)", [&] {
    // Refuses everyone, as an elevated game's object refuses a normal app
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;GA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr);
    SECURITY_ATTRIBUTES security {sizeof(security), descriptor, FALSE};
    const auto name = SharedObjectName(SharedAbi::BlockObject, SharedAbi::AbiVersion);
    HANDLE squatter = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, 4096, name.c_str());
    LocalFree(descriptor);
    if (!Check(squatter != nullptr, "the name taken first, with a DACL that refuses everyone")) {
      return;
    }
    auto host = Start(L"--exit-after 25");
    Check(LogShows("Shared memory access denied: another program holds it. If a game runs as administrator", 15),
      "the host says another program holds it, and what to do");
    Check(WaitForWindow() && RunCommand({L"recenter"}) == 0, "it carries on (a command answered)");
    CloseHandle(squatter);
    Check(LogShows("Shared memory created; layers can attach", 15), "once it's let go, the host makes the block");
    Check(WaitExit(host) == 0, "and quits normally");
  });

  Scenario("Started on a game's profile with no game running: back to the one it replaced (round 23)", [&] {
    // As an update leaves it: closed mid-game, on the game's profile, with
    // the profile it replaced remembered
    std::string defaultID;
    for (const auto& entry: fs::directory_iterator(gData / "profiles")) {
      const auto json = JsonUtil::Json::parse(ReadText(entry.path()), nullptr, false);
      if (entry.path().extension() == ".json" && json.is_object() && json.value("name", "") == "Default") {
        defaultID = json.value("id", "");
      }
    }
    auto settings = JsonUtil::Json::parse(ReadText(gData / "settings.json"), nullptr, false);
    if (!Check(!defaultID.empty() && settings.is_object(), "the profiles 'Default' and 'Second', and settings")) {
      return;
    }
    settings["activeProfile"] = "aaaaaaaaaaaaaaaa";
    settings["profileBeforeGame"] = defaultID;
    WriteText(gData / "settings.json", settings.dump(2));
    auto host = Start(L"--exit-after 20");
    Check(LogShows("active 'Second'", 10), "it starts on the game's profile, 'Second'");
    Check(LogShows("Profile: 'Default' (the game it was for isn't running)", 18),
      "with no game's layer in 10 s, it goes back to 'Default'");
    Check(WaitExit(host) == 0, "and quits normally");
    const auto saved = JsonUtil::Json::parse(ReadText(gData / "settings.json"), nullptr, false);
    Check(saved.is_object() && saved.value("activeProfile", "") == defaultID && !saved.contains("profileBeforeGame"),
      "saved: 'Default' active, nothing left to go back to");
  });

  return Finish();
}
