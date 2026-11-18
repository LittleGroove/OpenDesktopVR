// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Test helpers for real browsers (occlusion-test, browser-input-test):
// - Edge and Firefox, where they're installed, on a throwaway profile, with
//   a page this test writes; their processes in a job, closed with it;
// - the page talks back through its title ("odvr ..."), read without
//   sending the browser anything;
// - waiting keeps this thread's windows responsive.
#pragma once

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace OpenDesktopVR::Testing {

// Waits, keeping this thread's windows responsive (a window that stops
// answering is replaced by a ghost on screen)
inline void PumpFor(std::chrono::milliseconds duration) {
  const auto end = std::chrono::steady_clock::now() + duration;
  do {
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < end);
}

inline bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    PumpFor(std::chrono::milliseconds(50));
  }
  return condition();
}

// A window's title (another process's: the stored text, nothing sent)
inline std::wstring TitleOf(HWND window) {
  wchar_t text[512] {};
  GetWindowTextW(window, text, 512);
  return text;
}

struct BrowserKind {
  const char* mName;
  std::filesystem::path mExe;
  // Command-line arguments for a throwaway profile folder and a page URL
  std::function<std::wstring(const std::wstring& profile, const std::wstring& url)> mArguments;
  // Sets up a new profile folder before the first start; may be empty
  std::function<void(const std::filesystem::path& profile)> mPrepareProfile;
};

// Firefox on a new profile puts its welcome screens over the page, which
// then gets no input at all (measured, M4b): turned off
inline void QuietFirefoxProfile(const std::filesystem::path& profile) {
  std::filesystem::create_directories(profile);
  std::ofstream prefs {profile / L"user.js"};
  for (const char* pref: {
         R"(user_pref("browser.aboutwelcome.enabled", false);)",
         R"(user_pref("browser.shell.checkDefaultBrowser", false);)",
         R"(user_pref("browser.startup.homepage_override.mstone", "ignore");)",
         R"(user_pref("startup.homepage_welcome_url", "");)",
         R"(user_pref("trailhead.firstrun.didSeeAboutWelcome", true);)",
         R"(user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);)",
         R"(user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);)",
         R"(user_pref("app.normandy.enabled", false);)",
         R"(user_pref("messaging-system.rsexperimentloader.enabled", false);)",
         R"(user_pref("browser.uitour.enabled", false);)",
       }) {
    prefs << pref << '\n';
  }
}

// Firefox and Edge, at their usual install paths
inline std::vector<BrowserKind> Browsers() {
  wchar_t programs[MAX_PATH] {};
  GetEnvironmentVariableW(L"ProgramFiles", programs, MAX_PATH);
  wchar_t programs86[MAX_PATH] {};
  GetEnvironmentVariableW(L"ProgramFiles(x86)", programs86, MAX_PATH);
  return {
    {"Firefox", std::filesystem::path {programs} / L"Mozilla Firefox" / L"firefox.exe",
      [](const std::wstring& profile, const std::wstring& url) {
        return std::format(L"-profile \"{}\" -no-remote -new-instance \"{}\"", profile, url);
      },
      &QuietFirefoxProfile},
    {"Edge", std::filesystem::path {programs86} / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe",
      [](const std::wstring& profile, const std::wstring& url) {
        return std::format(
          L"--user-data-dir=\"{}\" --no-first-run --no-default-browser-check --new-window \"{}\"", profile, url);
      }},
  };
}

// The browser: its processes in a job, closed with it
class Browser {
 public:
  Browser(const std::filesystem::path& exe, const std::wstring& arguments) {
    mJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(mJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + arguments;
    STARTUPINFOW startup {sizeof(startup)};
    PROCESS_INFORMATION process {};
    if (CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr,
          &startup, &process)) {
      AssignProcessToJobObject(mJob, process.hProcess);
      ResumeThread(process.hThread);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      mStarted = true;
    }
  }
  ~Browser() {
    // The process that owns the window (it may have left the job), then the rest
    if (mWindow) {
      DWORD id = 0;
      GetWindowThreadProcessId(mWindow, &id);
      if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, id)) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
      }
    }
    CloseHandle(mJob);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  Browser(const Browser&) = delete;
  Browser& operator=(const Browser&) = delete;

  bool Started() const {
    return mStarted;
  }
  // Its page's window, once the page has set its title to start with "odvr "
  HWND FindPage(std::chrono::milliseconds limit) {
    WaitUntil([&] {
      EnumWindows(
        [](HWND window, LPARAM context) -> BOOL {
          auto* self = reinterpret_cast<Browser*>(context);
          if (IsWindowVisible(window) && TitleOf(window).starts_with(L"odvr ")) {
            self->mWindow = window;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
      return mWindow != nullptr;
    }, limit);
    return mWindow;
  }

 private:
  HANDLE mJob {nullptr};
  bool mStarted {false};
  HWND mWindow {nullptr};
};

}// namespace OpenDesktopVR::Testing
