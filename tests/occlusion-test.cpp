// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The app helper with real browsers (CT-3, HK-1, HK-7, HK-9; ADR-031).
//
// Each browser runs on a throwaway profile with a page that puts
// document.visibilityState and its animation-frame count in its title:
// - covered by an ordinary window (this process's, with the foreground): it
//   hides within about a second. Firefox doesn't count popup-style windows
//   as covering it. If it doesn't hide on this PC, its scenario is skipped
//   (nothing to fix here);
// - the helper on, still covered: visible again without any input (HK-7),
//   and it keeps animating;
// - the helper off: it hides again; the hook is gone; nothing was answered
//   twice (HK-3).
// A browser that isn't installed is skipped. Only the processes this test
// started are closed.

#include "browser.hpp"
#include "check.hpp"
#include "foreground.hpp"

#include <OpenDesktopVR/AppHelperService.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

using namespace OpenDesktopVR;
using namespace std::chrono_literals;
using AppHelper::AppHelperService;
using Testing::Check;
using Testing::Scenario;

namespace {

constexpr char Page[] = R"(<!doctype html>
<title>odvr init</title>
<body style="margin:0;background:#000"><canvas id="c" width="300" height="200"></canvas>
<script>
let n = 0;
const c = document.getElementById('c').getContext('2d');
function f() { n++; c.fillStyle = `hsl(${n % 360},80%,50%)`; c.fillRect(0, 0, 300, 200); requestAnimationFrame(f); }
requestAnimationFrame(f);
setInterval(() => { document.title = `odvr ${document.visibilityState} ${n}`; }, 100);
</script></body>
)";

struct PageState {
  bool mVisible {false};
  long mFrames {0};
};

// "odvr visible 123 - Microsoft Edge"
std::optional<PageState> ReadPage(HWND window) {
  wchar_t text[256] {};
  GetWindowTextW(window, text, 256);
  const std::wstring title {text};
  if (!title.starts_with(L"odvr ")) {
    return std::nullopt;
  }
  PageState state;
  state.mVisible = title.starts_with(L"odvr visible ");
  const auto number = title.find_first_of(L"0123456789");
  if (number != std::wstring::npos) {
    state.mFrames = std::wcstol(title.c_str() + number, nullptr, 10);
  }
  return state;
}

// Shared with browser-input-test (support/browser.hpp, foreground.hpp)
using Testing::Browser;
using Testing::TakeForeground;

void Pump(std::chrono::milliseconds duration) {
  Testing::PumpFor(duration);
}

bool WaitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
  return Testing::WaitUntil(condition, limit);
}

LRESULT CALLBACK CoverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  return DefWindowProcW(window, message, wParam, lParam);
}

// An ordinary opaque window over `target`, activated, like a game over a browser
HWND Cover(HWND target) {
  static const ATOM registered = [] {
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = &CoverProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
    windowClass.lpszClassName = L"OdvrOcclusionTestCover";
    return RegisterClassW(&windowClass);
  }();
  (void)registered;
  RECT rect {};
  GetWindowRect(target, &rect);
  // An ordinary window: Firefox doesn't count popup windows as covering it
  const HWND cover = CreateWindowExW(0, L"OdvrOcclusionTestCover", L"Occlusion test cover", WS_OVERLAPPED | WS_VISIBLE,
    rect.left - 20, rect.top - 20, rect.right - rect.left + 40, rect.bottom - rect.top + 40, nullptr, nullptr,
    GetModuleHandleW(nullptr), nullptr);
  std::printf("  cover %s the foreground\n", TakeForeground(cover) ? "has" : "doesn't have");
  return cover;
}

void RunBrowser(const char* name, const std::filesystem::path& exe, const std::function<std::wstring(const std::wstring&, const std::wstring&)>& arguments) {
  if (!std::filesystem::exists(exe)) {
    std::printf("  (skipped: %s isn't installed here)\n", name);
    return;
  }
  Testing::TempFolder folder;
  const auto profile = (std::filesystem::path {folder.Path()} / L"profile").wstring();
  const auto page = std::filesystem::path {folder.Path()} / L"page.html";
  std::filesystem::create_directories(profile);
  std::ofstream {page} << Page;
  std::wstring url = L"file:///" + page.generic_wstring();

  AppHelperService service;
  Browser browser {exe, arguments(profile, url)};
  if (!Check(browser.Started(), std::format("{} starts", name))) {
    return;
  }
  const HWND window = browser.FindPage(30s);
  if (!Check(window != nullptr, "its page's window appears")) {
    return;
  }
  SetWindowPos(window, nullptr, 60, 60, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE);
  // It may have opened behind other windows (started from a console)
  TakeForeground(window);
  Check(WaitFor([&] { return ReadPage(window) && ReadPage(window)->mVisible; }, 10s), "the page animates, uncovered");

  const HWND cover = Cover(window);
  const auto start = std::chrono::steady_clock::now();
  const bool hid = WaitFor([&] { return ReadPage(window) && !ReadPage(window)->mVisible; }, 6s);
  if (!hid) {
    std::printf("  (skipped: %s doesn't stop when covered on this PC)\n", name);
    DestroyWindow(cover);
    return;
  }
  std::printf("  covered: hidden after %.1f s\n",
    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());

  // HK-1, HK-7: on while it's already hidden, and still covered
  auto lease = service.KeepPainting(1, window);
  const bool on = WaitFor([&] {
    const auto status = service.StatusOf(1);
    return status && status->mState != AppHelperService::State::Waiting;
  }, 8s);
  const auto status = service.StatusOf(1);
  Check(on && status && status->mState == AppHelperService::State::On,
    std::format("the helper is on ({})", status ? status->mText : std::string {"no status"}));
  Check(WaitFor([&] { return ReadPage(window) && ReadPage(window)->mVisible; }, 5s),
    "still covered, it's visible again, without any input (HK-7)");
  const long before = ReadPage(window) ? ReadPage(window)->mFrames : 0;
  Pump(5s);
  const auto after = ReadPage(window);
  Check(after && after->mVisible, "after 5 s covered: still visible (CT-3)");
  const long frames = after ? after->mFrames - before : 0;
  Check(frames >= 50, std::format("and still animating: {} frames in 5 s", frames));

  // Off: the real visibility again, and the helper unloaded
  lease.reset();
  Check(WaitFor([&] { return service.Installs() == 0; }, 5s), "the helper unloads when let go (HK-9)");
  Check(WaitFor([&] { return ReadPage(window) && !ReadPage(window)->mVisible; }, 6s), "and the covered page hides again");
  Check(service.UnexpectedReplies() == 0, "no command was answered twice (HK-3)");
  DestroyWindow(cover);
}

}// namespace

int main() {
  wchar_t programs[MAX_PATH] {};
  GetEnvironmentVariableW(L"ProgramFiles", programs, MAX_PATH);
  wchar_t programs86[MAX_PATH] {};
  GetEnvironmentVariableW(L"ProgramFiles(x86)", programs86, MAX_PATH);

  Scenario("Firefox keeps painting while covered (CT-3)", [&] {
    RunBrowser("Firefox", std::filesystem::path {programs} / L"Mozilla Firefox" / L"firefox.exe",
      [](const std::wstring& profile, const std::wstring& url) {
        return std::format(L"-profile \"{}\" -no-remote -new-instance \"{}\"", profile, url);
      });
  });

  Scenario("Edge keeps painting while covered (CT-3)", [&] {
    RunBrowser("Edge", std::filesystem::path {programs86} / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe",
      [](const std::wstring& profile, const std::wstring& url) {
        return std::format(L"--user-data-dir=\"{}\" --no-first-run --no-default-browser-check --new-window \"{}\"",
          profile, url);
      });
  });

  return Testing::Finish();
}
