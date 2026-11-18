// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The real host moving a version 1 settings.json's panels into a profile
// (M6a, ADR-038), in a private data folder (OPENDESKTOPVR_DATA_FOLDER), so
// no real settings are touched:
// - settings.json becomes version 2, without "panels", with the active
//   profile's ID; everything else in it is kept (ST-5);
// - the old file is backed up first;
// - the profile "Default" holds both panels, a field this version doesn't
//   know included;
// - a second start changes nothing: no new profile, the same one active.
// A fresh install (M6f, round 22): the installer writes settings.json's
// layer part (odvr-layer test-panel off) before the host first starts; that
// start is still a first start (the settings window opens, with a welcome),
// and the next one isn't. Neither is a version 1 file's.
//
//   settings-migration-test <host exe> <odvr-layer exe>

#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>

#include <Windows.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace OpenDesktopVR;
using Testing::Check;
using Testing::Scenario;
namespace fs = std::filesystem;

namespace {

constexpr char Version1[] = R"({"schemaVersion": 1,
 "panels": [
  {"id": "00000000000000a1", "name": "Discord", "kind": "window",
   "window": {"executable": "", "class": "", "title": "Discord", "titleMode": "contains"},
   "position": [0.3, 0.1, -0.7], "widthMeters": 0.4, "enabled": true, "futureField": 42},
  {"id": "00000000000000a2", "name": "Notes", "kind": "testPattern", "animated": false,
   "position": [-0.3, 0.1, -0.7], "widthMeters": 0.3}
 ],
 "hands": {"enabled": true, "tapOffsetUpMillimeters": 4},
 "layer": {"enabled": true},
 "somethingNewer": {"keep": true}})";

// Runs the host for a few seconds in `folder`
bool RunHost(const fs::path& host, const fs::path& folder) {
  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", folder.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", std::format(L"migration-{}", GetCurrentProcessId()).c_str());
  std::wstring command = std::format(L"\"{}\" --no-register --warp --exit-after 3", host.wstring());
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION process {};
  if (!CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    return false;
  }
  const bool exited = WaitForSingleObject(process.hProcess, 30'000) == WAIT_OBJECT_0;
  if (!exited) {
    TerminateProcess(process.hProcess, 1);
  }
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return exited;
}

// Runs `odvr-layer <words>` in `folder`, as the installer does; its exit code
int RunLayerTool(const fs::path& tool, const fs::path& folder, const std::wstring& words) {
  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", folder.c_str());
  std::wstring command = std::format(L"\"{}\" {}", tool.wstring(), words);
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION process {};
  if (!CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
        &process)) {
    return -1;
  }
  DWORD code = static_cast<DWORD>(-1);
  if (WaitForSingleObject(process.hProcess, 30'000) == WAIT_OBJECT_0) {
    GetExitCodeProcess(process.hProcess, &code);
  } else {
    TerminateProcess(process.hProcess, 1);
  }
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return static_cast<int>(code);
}

// The host log's lines from its last start
std::string LastStartLog(const fs::path& folder) {
  const auto text = FileUtil::ReadAll(folder / L"logs" / L"host.log").value_or("");
  const auto at = text.rfind("log started");
  return at == std::string::npos ? std::string {} : text.substr(at);
}

constexpr char FirstStart[] = "First start: no profiles";

std::optional<JsonUtil::Json> ReadJson(const fs::path& path) {
  const auto text = FileUtil::ReadAll(path);
  return text ? JsonUtil::Parse(*text) : std::nullopt;
}

std::vector<fs::path> Profiles(const fs::path& folder) {
  std::vector<fs::path> files;
  std::error_code error;
  for (const auto& entry: fs::directory_iterator(folder / L"profiles", error)) {
    if (entry.path().extension() == L".json") {
      files.push_back(entry.path());
    }
  }
  return files;
}

}// namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("Usage: settings-migration-test <host exe> <odvr-layer exe>\n");
    return 2;
  }
  const fs::path host = fs::absolute(argv[1]);
  const fs::path layerTool = fs::absolute(argv[2]);
  Testing::TempFolder temp;
  const fs::path folder {temp.Path()};
  std::ofstream {folder / L"settings.json", std::ios::binary} << Version1;

  std::string activeID;
  Scenario("Version 1's panels move into the profile 'Default' (ADR-038)", [&] {
    Check(RunHost(host, folder), "the host ran and exited by itself");
    const auto settings = ReadJson(folder / L"settings.json");
    if (!Check(settings.has_value(), "settings.json is still readable")) {
      return;
    }
    Check(JsonUtil::GetOr<int>(*settings, "schemaVersion", 0) == 2, "it's version 2");
    Check(!settings->contains("panels"), "without the panel list");
    activeID = JsonUtil::GetOr<std::string>(*settings, "activeProfile", "");
    Check(activeID.size() == 16, std::format("with the active profile's ID ({})", activeID));
    Check(settings->contains("somethingNewer") && JsonUtil::GetOr<int>((*settings)["hands"], "tapOffsetUpMillimeters", 0) == 4,
      "everything else kept (ST-5)");
    bool backup = false;
    for (const auto& entry: fs::directory_iterator(folder)) {
      const auto name = entry.path().filename().wstring();
      backup = backup || (name.starts_with(L"settings.json.") && name != L"settings.json");
    }
    Check(backup, "the version 1 file backed up beside it");

    const auto profiles = Profiles(folder);
    Check(profiles.size() == 1, std::format("one profile ({})", profiles.size()));
    const auto profile = profiles.empty() ? std::nullopt : ReadJson(profiles.front());
    if (!Check(profile.has_value(), "its file reads")) {
      return;
    }
    const auto panels = JsonUtil::GetOr<JsonUtil::Json>(*profile, "panels", JsonUtil::Json::array());
    Check(JsonUtil::GetOr<std::string>(*profile, "name", "") == "Default" && panels.size() == 2,
      std::format("'Default', with both panels ({})", panels.size()));
    bool kept = false;
    for (const auto& panel: panels) {
      kept = kept || JsonUtil::GetOr<int>(panel, "futureField", 0) == 42;
    }
    Check(kept, "a panel field this version doesn't know survived the move");
    Check(LastStartLog(folder).find(FirstStart) == std::string::npos, "an upgrade isn't a first start");
  });

  Scenario("A second start changes nothing", [&] {
    Check(RunHost(host, folder), "the host ran again");
    const auto settings = ReadJson(folder / L"settings.json");
    Check(settings && JsonUtil::GetOr<std::string>(*settings, "activeProfile", "") == activeID, "the same profile is active");
    Check(Profiles(folder).size() == 1, "still one profile");
  });

  Testing::TempFolder freshTemp;
  const fs::path fresh {freshTemp.Path()};
  Scenario("A fresh install: the installer writes layer settings first; the host's first start is still one (round 22)", [&] {
    Check(RunLayerTool(layerTool, fresh, L"test-panel off") == 0, "odvr-layer test-panel off, as install does");
    const auto installed = ReadJson(fresh / L"settings.json");
    if (!Check(installed && installed->contains("layer") && !installed->contains("activeProfile"),
          "it wrote settings.json, with only the layer's part")) {
      return;
    }
    Check(RunHost(host, fresh), "the host ran");
    Check(LastStartLog(fresh).find(FirstStart) != std::string::npos, "its start is a first start (the settings window opens)");
    Check(Profiles(fresh).size() == 1, "it made the profile 'Default'");
    Check(RunHost(host, fresh), "the host ran again");
    const auto again = LastStartLog(fresh);
    Check(!again.empty() && again.find(FirstStart) == std::string::npos, "the second start isn't a first start");
  });

  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", nullptr);
  return Testing::Finish();
}
