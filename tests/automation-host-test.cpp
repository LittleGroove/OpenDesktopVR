// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Automation end to end (M6e, UI-9, ADR-042): the real host in a private
// instance and data folder, and the real program run as a command, as
// Stream Deck would run it:
// - with no host, a command says so (exit code 2) and doesn't start one;
// - commands are done (exit code 0): hide by name, toggle by place,
//   opacity and brightness, nudge with --repeat, the keyboard nudged, tint,
//   profile next, the quick menu, show all;
// - nonsense is refused (exit code 1) and logged with the reason, and
//   raw messages that aren't ours are refused too (TH-9);
// - 200 commands at once: each done or "busy", never lost silently, and
//   the host still answers after;
// - the host quits normally, and what the commands changed was saved.
//
//   automation-host-test <OpenDesktopVR.exe>

#include "check.hpp"

#include <OpenDesktopVR/Automation.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;

namespace {

std::wstring gHostExe;

std::wstring Quoted(const std::wstring& word) {
  return word.find_first_of(L" \t") == std::wstring::npos && !word.empty() ? word : L"\"" + word + L"\"";
}

// Runs the program as a command; its exit code (-1 if it didn't end)
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
  } else {
    TerminateProcess(info.hProcess, 99);
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return static_cast<int>(code);
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream file {path, std::ios::binary};
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::printf("usage: automation-host-test <host exe>\n");
    return 2;
  }
  gHostExe = argv[1];
  const auto instance = std::format(L"aut{}", GetCurrentProcessId());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", instance.c_str());
  TempFolder data;
  SetEnvironmentVariableW(L"OPENDESKTOPVR_DATA_FOLDER", data.Path().c_str());
  auto windowClass = SharedObjectName(L"Automation", SharedAbi::AbiVersion);
  windowClass.erase(0, windowClass.find(L'\\') + 1);

  Scenario("No host running: a command says so and starts nothing", [&] {
    const int code = RunCommand({L"toggle", L"all"});
    Check(code == static_cast<int>(Automation::Reply::NotRunning), std::format("exit code 2, not running ({})", code));
    Check(!FindWindowExW(HWND_MESSAGE, nullptr, windowClass.c_str(), nullptr), "no host was started by it");
  });

  // The host: a private instance and data folder (the test panels in a
  // fresh "Default" profile), quitting by itself
  auto hostLine = std::format(L"\"{}\" --no-register --warp --static-panels --exit-after 30", gHostExe);
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION host {};
  if (!Check(CreateProcessW(gHostExe.c_str(), hostLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &host) != 0,
        "the host starts")) {
    return Finish();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  HWND window = nullptr;
  while (!window && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    window = FindWindowExW(HWND_MESSAGE, nullptr, windowClass.c_str(), nullptr);
  }
  Check(window != nullptr, "its command window appears");

  Scenario("Commands are done (exit code 0)", [&] {
    const std::vector<std::vector<std::wstring>> commands {
      {L"hide", L"Animated test pattern"},
      {L"toggle", L"#2"},
      {L"toggle", L"#2"},
      {L"opacity", L"#1", L"-30"},
      {L"brightness", L"all", L"50"},
      {L"nudge", L"#1", L"left", L"5", L"--repeat", L"2"},
      {L"nudge", L"keyboard", L"up", L"2"},
      {L"tint", L"Static test pattern", L"on"},
      {L"profile", L"next"},
      {L"menu", L"open"},
      {L"show", L"all"},
      {L"recenter"},
      {L"settings"},// what starting the program again sends (M6f)
    };
    for (const auto& words: commands) {
      std::wstring text;
      for (const auto& word: words) {
        text += L" " + word;
      }
      const int code = RunCommand(words);
      Check(code == 0, std::format("{} (exit code {})", StringUtil::Narrow(text), code));
    }
  });

  Scenario("Nonsense is refused (exit code 1) and logged (TH-9)", [&] {
    const std::vector<std::vector<std::wstring>> commands {
      {L"toogle", L"all"},
      {L"show", L"No such panel"},
      {L"opacity", L"#1", L"lots"},
      {L"nudge", L"#1", L"sideways"},
      {L"toggle", std::wstring(300, L'x')},
      {L"window", L"#1", L"Anything"},// a test pattern shows no window
      {L"profile", L"Nobody's"},
    };
    for (const auto& words: commands) {
      const int code = RunCommand(words);
      Check(code == static_cast<int>(Automation::Reply::Refused),
        std::format("{} ... refused (exit code {})", StringUtil::Narrow(words[0]), code));
    }
    // Messages that aren't ours, straight to the window
    const auto send = [&](uintptr_t tag, const void* bytes, DWORD size) {
      COPYDATASTRUCT copy {tag, size, const_cast<void*>(bytes)};
      DWORD_PTR result = 0;
      SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy), SMTO_ABORTIFHUNG, 3000, &result);
      return result;
    };
    const wchar_t good[] = L"recenter";
    Check(send(Automation::WireTag + 7, good, sizeof(good)) == 101, "another tag: refused");
    Check(send(Automation::WireTag, good, sizeof(good) - 1) == 101, "an odd size: refused");
    Check(send(Automation::WireTag, good, sizeof(good) - sizeof(wchar_t)) == 101, "no final NUL: refused");
    Check(send(Automation::WireTag, nullptr, 0) == 101, "nothing: refused");
    Check(send(Automation::WireTag, good, sizeof(good)) == 100, "and a good one is still done");
  });

  Scenario("200 commands at once: each done or busy, the host still answering", [&] {
    const auto words = Automation::Pack(std::vector<std::wstring> {L"nudge", L"#2", L"right", L"0.1"});
    int done = 0;
    int busy = 0;
    for (int i = 0; i < 200; ++i) {
      COPYDATASTRUCT copy {Automation::WireTag, static_cast<DWORD>(words.size() * sizeof(wchar_t)), const_cast<wchar_t*>(words.data())};
      DWORD_PTR result = 0;
      SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy), SMTO_ABORTIFHUNG, 3000, &result);
      done += result == 100 ? 1 : 0;
      busy += result == 103 ? 1 : 0;
    }
    Check(done + busy == 200 && done >= 64, std::format("{} done, {} busy (64 can wait at once), none lost", done, busy));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Check(RunCommand({L"recenter"}) == 0, "the host answers after");
  });

  Scenario("The host quits normally; what the commands changed was saved", [&] {
    const bool exited = WaitForSingleObject(host.hProcess, 60'000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (exited) {
      GetExitCodeProcess(host.hProcess, &code);
    } else {
      TerminateProcess(host.hProcess, 1);
    }
    Check(exited && code == 0, std::format("the host quit by itself, exit code {}", code));
    const auto folder = std::filesystem::path {data.Path()};
    const auto log = ReadText(folder / "logs" / "host.log");
    Check(log.find("Command: hide 'Animated test pattern': 'Animated test pattern' hidden") != std::string::npos,
      "logged: hide 'Animated test pattern'");
    Check(log.find("Command refused: unknown command 'toogle'") != std::string::npos, "logged: the unknown command, with why");
    Check(log.find("no panel called 'No such panel'") != std::string::npos, "logged: the unknown panel");
    Check(log.find("Quick menu: opened (a command)") != std::string::npos, "the quick menu opened");

    // The profile and settings, as saved
    JsonUtil::Json profile;
    for (const auto& entry: std::filesystem::directory_iterator(folder / "profiles")) {
      profile = JsonUtil::Json::parse(ReadText(entry.path()), nullptr, false);
    }
    const auto panels = profile.is_object() ? profile.value("panels", JsonUtil::Json::array()) : JsonUtil::Json::array();
    const JsonUtil::Json* animated = nullptr;
    const JsonUtil::Json* still = nullptr;
    for (const auto& panel: panels) {
      if (panel.value("name", "") == "Animated test pattern") {
        animated = &panel;
      } else if (panel.value("name", "") == "Static test pattern") {
        still = &panel;
      }
    }
    if (!Check(animated && still, "both test panels saved")) {
      return;
    }
    const float x = (*animated)["position"][0].get<float>();
    Check(std::abs((*animated)["opacity"].get<float>() - 0.7f) < 0.001f, "opacity #1 -30: 70%");
    Check(std::abs(x - (-0.32f)) < 0.0005f, std::format("nudge #1 left 5 cm, twice: -22 cm to {:.1f} cm", x * 100));
    Check(std::abs((*still)["brightness"].get<float>() - 0.5f) < 0.001f && (*still)["tintEnabled"].get<bool>(),
      "brightness all 50, tint on");
    const float stillX = (*still)["position"][0].get<float>();
    Check(stillX > 0.22f && stillX < 0.22f + 0.2f, std::format("the 200 nudges right moved #2 by what was done ({:.1f} cm)", stillX * 100));
    const auto settings = JsonUtil::Json::parse(ReadText(folder / "settings.json"), nullptr, false);
    const float keyboardY = settings.is_object() ? settings["keyboard"]["position"][1].get<float>() : 0;
    Check(std::abs(keyboardY - (-0.28f)) < 0.0005f, std::format("nudge keyboard up 2: -30 cm to {:.1f} cm", keyboardY * 100));
  });

  CloseHandle(host.hThread);
  CloseHandle(host.hProcess);
  return Finish();
}
