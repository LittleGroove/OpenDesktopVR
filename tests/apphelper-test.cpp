// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The app helper (doc 06; ADR-031), against a test app in another process
// (apphelper-test-app) that behaves like a Chromium app:
// - which apps need it (Chromium-based, Firefox), and the reply format;
// - import-table patching in this process: regular imports, again, restored;
// - in the app (HK-1, HK-6, HK-7): other processes' top-level windows look
//   hidden, through its own imports and a delay-loaded module; child windows,
//   the desktop and its own windows don't change; it's told to look again;
// - one hook per UI thread for two of its windows (HK-2);
// - nothing answered twice, though the app peeks before taking (HK-3);
// - unloading: everything back, the DLL gone from the app, 15 times while a
//   thread calls the check as fast as it can (HK-9, R8);
// - never the game, never the host itself (HK-11);
// - the service stopping with a window still on unloads it too.
// The real browsers are in occlusion-test.

#include "apphelper-probe.hpp"
#include "check.hpp"

#include <OpenDesktopVR/AppHelperProtocol.hpp>
#include <OpenDesktopVR/AppHelperRules.hpp>
#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/ImportPatch.hpp>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::AppHelper;
using namespace std::chrono_literals;
using Testing::AppHelperProbe;
using Testing::Check;
using Testing::Scenario;

namespace {

std::atomic<int> gFakeCalls {0};

BOOL WINAPI FakeIsWindowVisible(HWND) {
  ++gFakeCalls;
  return FALSE;
}

// Not inlined, so every call reads the import table again
__declspec(noinline) BOOL CallIsWindowVisible(HWND window) {
  return IsWindowVisible(window);
}

void Pump(std::chrono::milliseconds duration) {
  const auto end = std::chrono::steady_clock::now() + duration;
  do {
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < end);
}

bool WaitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    Pump(10ms);
  }
  return condition();
}

// The app's answers are fresh once two more rounds have run
void Settle(const AppHelperProbe& probe) {
  const auto rounds = probe.mRounds.load();
  WaitFor([&] { return probe.mRounds.load() >= rounds + 2; }, 2s);
}

bool IsOn(const AppHelperService& service, uint64_t panel) {
  return WaitFor([&] {
    const auto status = service.StatusOf(panel);
    return status && status->mState != AppHelperService::State::Waiting;
  }, 8s) && service.StatusOf(panel)->mState == AppHelperService::State::On;
}

HWND MakeWindow(const wchar_t* className, HWND parent, int x, int y, int width, int height) {
  WNDCLASSW windowClass {};
  windowClass.lpfnWndProc = &DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
  windowClass.lpszClassName = className;
  RegisterClassW(&windowClass);
  return CreateWindowExW(0, className, L"apphelper-test", (parent ? WS_CHILD : WS_OVERLAPPED) | WS_VISIBLE, x, y,
    width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

}// namespace

int main(int argc, char** argv) {
  Scenario("Which apps need the helper", [] {
    Check(FamilyOf(L"Chrome_WidgetWin_1") == AppFamily::Chromium, "Chrome_WidgetWin_1 (Edge, Chrome, Discord, Spotify): Chromium-based");
    Check(FamilyOf(L"Chrome_WidgetWin_0") == AppFamily::Chromium, "Chrome_WidgetWin_0 too");
    Check(FamilyOf(L"MozillaWindowClass") == AppFamily::Firefox, "MozillaWindowClass: Firefox");
    Check(FamilyOf(L"Notepad") == AppFamily::None, "Notepad: none");
    Check(FamilyOf(L"Chrome_RenderWidgetHostHWND") == AppFamily::None, "a Chromium child window's class: none");
  });

  Scenario("Replies pack and unpack", [] {
    const AppHelperProtocol::Reply reply {AppHelperProtocol::Command::KeepPaintingOn, AppHelperProtocol::Result::NothingToPatch, 4321};
    const auto back = AppHelperProtocol::Unpack(AppHelperProtocol::Pack(reply));
    Check(back.mCommand == reply.mCommand && back.mResult == reply.mResult && back.mDetail == reply.mDetail, "the same reply comes back");
  });

  Scenario("Import patching in this process: redirected, again, restored", [] {
    const HWND desktop = GetDesktopWindow();
    auto* real = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowVisible"));
    Check(CallIsWindowVisible(desktop), "before: the desktop is visible");
    ImportPatch patch;
    const auto slots = patch.Apply("IsWindowVisible", real, reinterpret_cast<void*>(&FakeIsWindowVisible), nullptr);
    Check(slots >= 1, std::format("this exe's import of IsWindowVisible is redirected ({} slot{})", slots, slots == 1 ? "" : "s"));
    Check(!CallIsWindowVisible(desktop) && gFakeCalls > 0, "calls through it reach the replacement");
    Check(patch.Apply("IsWindowVisible", real, reinterpret_cast<void*>(&FakeIsWindowVisible), nullptr) == slots,
      "applying again changes nothing");
    Check(patch.Restore() == slots, "every slot is put back");
    const int calls = gFakeCalls;
    Check(CallIsWindowVisible(desktop) && gFakeCalls == calls, "after: the real one again");
  });

  const std::filesystem::path app = argc > 1 ? argv[1] : "apphelper-test-app.exe";

  // The test's own windows: a top-level one (another process, to the app)
  // and a child of it
  const HWND cover = MakeWindow(L"OdvrAppHelperTestCover", nullptr, 0, 160, 300, 200);
  const HWND coverChild = MakeWindow(L"OdvrAppHelperTestCoverChild", cover, 10, 10, 100, 60);
  const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(AppHelperProbe),
    Testing::AppHelperProbeName);
  auto* probe = new (MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AppHelperProbe))) AppHelperProbe {};
  probe->mCover = reinterpret_cast<uint64_t>(cover);
  probe->mCoverChild = reinterpret_cast<uint64_t>(coverChild);

  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION process {};
  std::wstring commandLine = L"\"" + app.wstring() + L"\"";
  const bool started = CreateProcessW(app.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
    &startup, &process);
  Check(started, "the test app starts");
  const bool ready = started && WaitFor([&] { return probe->mMainWindow != 0 && probe->mRounds > 2; }, 10s);
  Check(ready, "its windows are up and it's probing");
  const auto main = reinterpret_cast<HWND>(probe->mMainWindow);
  const auto second = reinterpret_cast<HWND>(probe->mSecondWindow);
  const DWORD appProcess = process.dwProcessId;

  if (ready) {
    Scenario("Before: the app sees every window as it is", [&] {
      Settle(*probe);
      Check(probe->mCoverVisible == 1 && probe->mCoverVisibleDelayLoaded == 1, "the test's window: visible (both ways)");
      Check(probe->mCoverChildVisible == 1 && probe->mDesktopVisible == 1, "its child and the desktop: visible");
      Check(probe->mOwnVisible == 1 && probe->mOwnHidden == 0, "its own: visible and hidden as they are");
      Check(probe->mHelperLoaded == 0, "no helper in it");
    });

    AppHelperService service;
    Scenario("On: other processes' top-level windows look hidden to the app, nothing else (HK-1, HK-6, HK-7)", [&] {
      const auto changes = probe->mLocationChanges.load();
      auto lease = service.KeepPainting(1, main);
      const bool on = IsOn(service, 1);
      Check(on, std::format("on ({})", service.StatusOf(1) ? service.StatusOf(1)->mText : ""));
      Settle(*probe);
      Check(probe->mCoverVisible == 0, "the test's window looks hidden, through the app's own imports");
      Check(probe->mCoverVisibleDelayLoaded == 0, "and through a module that delay-loads user32");
      Check(probe->mCoverChildVisible == 1, "a child window: unchanged (only top-level windows)");
      Check(probe->mDesktopVisible == 1, "the desktop window: unchanged");
      Check(probe->mOwnVisible == 1 && probe->mOwnHidden == 0, "its own windows: unchanged (a hidden one never shows)");
      Check(probe->mHelperLoaded == 1, "the helper is loaded in it");
      Check(probe->mLocationChanges.load() > changes, "it was told to look again (EVENT_OBJECT_LOCATIONCHANGE)");

      // HK-2: a second window of the same thread shares the hook
      auto lease2 = service.KeepPainting(2, second);
      Check(IsOn(service, 2), "its second window: on too");
      Check(service.Installs() == 1, "one hook for the thread, not one per window");
      lease.reset();
      Pump(300ms);
      Settle(*probe);
      Check(probe->mCoverVisible == 0 && service.Installs() == 1, "the first let go: still on for the second");

      lease2.reset();
      Check(WaitFor([&] { return service.Installs() == 0; }, 5s), "the last let go: the hook goes (HK-9)");
      Settle(*probe);
      Check(probe->mCoverVisible == 1 && probe->mCoverVisibleDelayLoaded == 1, "the app sees the real visibility again");
      Check(WaitFor([&] { return probe->mHelperLoaded == 0; }, 5s), "and the helper is gone from it (R8)");
    });

    Scenario("On and off 15 times while the app calls the check as fast as it can (HK-9)", [&] {
      const auto busyBefore = probe->mBusyCalls.load();
      int rounds = 0;
      for (int i = 0; i < 15; ++i) {
        auto lease = service.KeepPainting(3, main);
        if (!IsOn(service, 3)) {
          break;
        }
        Pump(std::chrono::milliseconds {20 * (i % 4)});
        lease.reset();
        if (!WaitFor([&] { return service.Installs() == 0; }, 5s)) {
          break;
        }
        ++rounds;
      }
      Check(rounds == 15, std::format("{} of 15 rounds on and off", rounds));
      Check(WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT, "the app is still running");
      Settle(*probe);
      Check(probe->mCoverVisible == 1, "it sees the real visibility");
      Check(probe->mBusyCalls.load() > busyBefore + 100000,
        std::format("its busy thread kept calling ({} calls)", probe->mBusyCalls.load() - busyBefore));
      Check(service.UnexpectedReplies() == 0, "nothing was answered twice, though it peeks first (HK-3)");
    });

    Scenario("Never the game, never this program (HK-11)", [&] {
      service.SetGameProcess([&] { return static_cast<uint32_t>(appProcess); });
      auto game = service.KeepPainting(4, main);
      Check(WaitFor([&] { return service.StatusOf(4) && service.StatusOf(4)->mState == AppHelperService::State::Unavailable; }, 3s),
        "a window of the game's process: never");
      service.SetGameProcess({});
      const HWND own = MakeWindow(L"Chrome_WidgetWin_OdvrSelf", nullptr, 320, 160, 200, 100);
      auto self = service.KeepPainting(5, own);
      Check(WaitFor([&] { return service.StatusOf(5) && service.StatusOf(5)->mState == AppHelperService::State::NotNeeded; }, 3s),
        "this program's own window: not needed");
      const HWND plain = MakeWindow(L"OdvrPlainWindow", nullptr, 320, 280, 200, 100);
      auto other = service.KeepPainting(6, plain);
      Check(WaitFor([&] { return service.StatusOf(6) && service.StatusOf(6)->mState == AppHelperService::State::NotNeeded; }, 3s),
        "a window of another kind: not needed");
      Check(service.Installs() == 0, "no hook for any of them");
      DestroyWindow(plain);
      DestroyWindow(own);
    });
  }

  if (ready) {
    Scenario("The service stops with a window still on: the helper unloads", [&] {
      std::shared_ptr<void> lease;
      {
        AppHelperService service;
        lease = service.KeepPainting(7, main);
        Check(IsOn(service, 7), "on");
        Settle(*probe);
        Check(probe->mCoverVisible == 0, "the test's window looks hidden");
      }// ~AppHelperService
      Settle(*probe);
      Check(probe->mCoverVisible == 1, "after: the real visibility");
      Check(WaitFor([&] { return probe->mHelperLoaded == 0; }, 5s), "the helper is gone from the app");
      lease.reset();// after the service: nothing happens
      Check(WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT, "the app is still running");
    });
  }

  probe->mStop = 1;
  if (started) {
    PostThreadMessageW(process.dwThreadId, WM_NULL, 0, 0);
    Check(WaitForSingleObject(process.hProcess, 10'000) == WAIT_OBJECT_0, "the test app exits");
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    Check(code == 0, "cleanly");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  UnmapViewOfFile(probe);
  CloseHandle(mapping);
  DestroyWindow(coverChild);
  DestroyWindow(cover);
  return Testing::Finish();
}
