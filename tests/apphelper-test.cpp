// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
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
// - input mode (M4b): inside an injected click the app sees the cursor at
//   the injected point (negative coordinates too), its window in the
//   foreground and active; another of its threads sees none of it (HK-5);
//   it can't come to the top or SendInput (HK-8); the fakes end 100 ms
//   after the end, 2 s after a lost end, and a stray end changes nothing;
// - keys (M5a): during an injected key-down, the virtual keyboard's Shift
//   and Caps Lock are what the app reads (GetKeyState, GetKeyboardState,
//   GetAsyncKeyState), and the key's characters are queued ahead of it with
//   the key's lParam (INJ-9); its real key state returns later, or at once
//   on the hand-back; "everything released" is a state, not the hand-back;
//   arguments that decode to nothing change nothing; at most 16 characters
//   are kept, and one whose key never came isn't typed later;
// - the service stopping with a window still on unloads it too.
// The real browsers are in occlusion-test.

#include "apphelper-probe.hpp"
#include "check.hpp"
#include "foreground.hpp"

#include <OpenDesktopVR/AppHelperProtocol.hpp>
#include <OpenDesktopVR/AppHelperRules.hpp>
#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/ImportPatch.hpp>
#include <OpenDesktopVR/InjectionHookProtocol.hpp>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <format>
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

  Scenario("A 32-bit app: a clear message, no helper, no crash (IN-6, doc 11)", [&] {
    // Windows' own 32-bit Character Map
    wchar_t windows[MAX_PATH] {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const auto exe = std::filesystem::path {windows} / L"SysWOW64" / L"charmap.exe";
    if (!std::filesystem::exists(exe)) {
      std::printf("  (skipped: no 32-bit Character Map here)\n");
      return;
    }
    std::wstring line = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW startup {sizeof(startup)};
    PROCESS_INFORMATION info {};
    if (!Check(CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info) != 0,
          "Character Map (32-bit) starts")) {
      return;
    }
    WaitForInputIdle(info.hProcess, 5000);
    struct Find {
      DWORD mProcess;
      HWND mWindow;
    } find {info.dwProcessId, nullptr};
    WaitFor([&] {
      EnumWindows([](HWND window, LPARAM data) {
        auto& wanted = *reinterpret_cast<Find*>(data);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == wanted.mProcess && IsWindowVisible(window)) {
          wanted.mWindow = window;
          return FALSE;
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&find));
      return find.mWindow != nullptr;
    }, 5s);
    if (Check(find.mWindow != nullptr, "its window is open")) {
      AppHelperService service;
      auto lease = service.Input(20, find.mWindow);
      std::string text;
      const bool unavailable = WaitFor([&] {
        const auto status = service.InputStatusOf(20);
        text = status ? status->mText : "";
        return status && status->mState == AppHelperService::State::Unavailable;
      }, 3s);
      Check(unavailable && text.find("32-bit") != std::string::npos, "its input status says why: " + text);
      Check(service.Installs() == 0, "no helper loaded into it");
    }
    TerminateProcess(info.hProcess, 0);
    WaitForSingleObject(info.hProcess, 5000);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
  });

  if (ready) {
    Scenario("Input mode: the app sees an injected click as real, only inside it (HK-1, HK-5, HK-8)", [&] {
      AppHelperService service;
      auto lease = service.Input(8, main);
      const bool on = WaitFor([&] {
        const auto status = service.InputStatusOf(8);
        return status && status->mState != AppHelperService::State::Waiting;
      }, 8s) && service.InputStatusOf(8)->mState == AppHelperService::State::On;
      Check(on, "input on: the helper loaded, its fakes set up");
      if (!on) {
        return;
      }
      const UINT command = RegisterWindowMessageW(AppHelperProtocol::CommandMessageName);
      const auto send = [&](UINT message, WPARAM wParam, LPARAM lParam) {
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(main, message, wParam, lParam, SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, &result) != 0;
      };
      const auto begin = [&](int x, int y) {
        send(command, static_cast<WPARAM>(AppHelperProtocol::Command::BeginInjection), AppHelperProtocol::PackPoint(x, y));
      };
      const auto end = [&] { send(command, static_cast<WPARAM>(AppHelperProtocol::Command::EndInjection), 0); };
      const auto probeForeground = [&] {
        const auto before = probe->mProbes.load();
        send(WM_APP + 1, 0, 0);
        return probe->mProbes.load() > before && probe->mProbeForeground == 1;
      };
      // The test's window is the foreground, like the game
      Check(Testing::TakeForeground(cover), "before: the test's window is the foreground");
      // The app's second window above its main one, so a click can try to
      // bring the main one up
      const auto secondOnTop = [&] {
        probe->mSecondOnTop = -1;
        send(WM_APP + 2, 0, 0);
        return probe->mSecondOnTop == 1;
      };
      // Active on its own thread first (activation reorders the app's own
      // windows by itself), then its second window on top
      begin(0, 0);
      send(command, static_cast<WPARAM>(InjectionHookProtocol::KeyCommand::ActivateOnThread), reinterpret_cast<LPARAM>(main));
      end();
      std::this_thread::sleep_for(150ms);
      Check(secondOnTop(), "before: the app's second window is above its main one");
      // Input the app sends: seen by a low-level hook here
      static std::atomic<int> injected {0};
      injected = 0;
      const HHOOK mouseHook = SetWindowsHookExW(WH_MOUSE_LL, [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (code == HC_ACTION && (reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam)->flags & LLMHF_INJECTED)) {
          ++injected;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
      }, GetModuleHandleW(nullptr), 0);

      // A click, as the injector sends it, left of and above the main monitor
      begin(-1234, -567);
      const auto downs = probe->mDowns.load();
      send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
      Check(probe->mDowns.load() == downs + 1, "the app handled the click");
      Check(probe->mDownCursorX == -1234 && probe->mDownCursorY == -567,
        std::format("GetCursorPos: the injected point ({}, {})", probe->mDownCursorX.load(), probe->mDownCursorY.load()));
      const auto packed = AppHelperProtocol::UnpackPoint(static_cast<LPARAM>(probe->mDownMessagePos.load()));
      Check(packed.x == -1234 && packed.y == -567, "GetMessagePos: the same, packed signed");
      Check(probe->mDownForeground == 1, "GetForegroundWindow: its own window");
      Check(probe->mDownActive == 1, "its window is active on its own thread");
      Check(probe->mOtherThreadCursorFaked == 0 && probe->mOtherThreadForegroundFaked == 0,
        "another of its threads sees the real cursor and foreground (HK-5)");
      Check(probe->mMainCameUp == 0, "it couldn't bring its window to the top (HK-8)");
      Check(GetForegroundWindow() == cover, "the real foreground didn't change");
      Pump(100ms);
      Check(injected == 0, "its SendInput went nowhere (HK-8)");
      Check(probeForeground(), "until the end, its window still looks foreground");
      end();
      Check(probeForeground(), "within 100 ms of the end: still");
      std::this_thread::sleep_for(150ms);
      Check(!probeForeground(), "150 ms after: the real foreground");

      // A stray end: never below zero, the next click works the same
      end();
      end();
      begin(50, 60);
      send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
      end();
      Check(probe->mDownCursorX == 50 && probe->mDownCursorY == 60, "after stray ends: a click works as before");
      std::this_thread::sleep_for(150ms);
      Check(!probeForeground(), "and ends 100 ms later (no count left over)");

      // An end that never comes
      begin(70, 80);
      Check(probeForeground(), "a begin with no end: foreground faked");
      std::this_thread::sleep_for(2200ms);
      Check(!probeForeground(), "2 s later: over by itself");
      begin(90, 100);
      end();
      std::this_thread::sleep_for(150ms);
      Check(!probeForeground(), "a begin after the expired one starts fresh: over 100 ms after its end");

      Check(GetForegroundWindow() == cover, "the test's window stayed the foreground throughout");
      lease.reset();
      Check(WaitFor([&] { return probe->mHelperLoaded == 0; }, 5s), "input off: the helper is gone from the app");
      // Without the helper, the app's own calls are real again
      Check(secondOnTop(), "after: the second window on top again");
      begin(1, 2);
      send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
      end();
      Check(probe->mDownCursorX != 1 || probe->mDownCursorY != 2, "after: the real cursor");
      Check(probe->mMainCameUp == 1, "after: the app can bring its window to the top again (the guard is gone)");
      Check(WaitFor([&] { return injected > 0; }, 1s), "after: its SendInput is real again (the hook here sees it)");
      UnhookWindowsHookEx(mouseHook);
    });

    Scenario("Keys: the keyboard's modifiers on the app's thread, characters with the key-down (doc 06, INJ-9)", [&] {
      AppHelperService service;
      auto lease = service.Input(9, main);
      const bool on = WaitFor([&] {
        const auto status = service.InputStatusOf(9);
        return status && status->mState != AppHelperService::State::Waiting;
      }, 8s) && service.InputStatusOf(9)->mState == AppHelperService::State::On;
      Check(on, "input on");
      if (!on) {
        return;
      }
      namespace Keys = InjectionHookProtocol;
      const UINT command = RegisterWindowMessageW(AppHelperProtocol::CommandMessageName);
      const auto send = [&](UINT message, WPARAM wParam, LPARAM lParam) {
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(main, message, wParam, lParam, SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, &result) != 0;
      };
      const auto keyCommand = [&](Keys::KeyCommand which, LPARAM argument) {
        send(command, static_cast<WPARAM>(which), argument);
      };
      const auto modifiers = [&](const Keys::InjectedModifiers& state) {
        keyCommand(Keys::KeyCommand::OverrideModifiers, Keys::EncodeModifiers(state));
      };
      const auto character = [&](wchar_t unit) {
        keyCommand(Keys::KeyCommand::CharacterForNextKey, Keys::EncodeCharacter(unit));
      };
      const auto begin = [&] {
        send(command, static_cast<WPARAM>(AppHelperProtocol::Command::BeginInjection), AppHelperProtocol::PackPoint(10, 10));
      };
      const auto end = [&] { send(command, static_cast<WPARAM>(AppHelperProtocol::Command::EndInjection), 0); };
      const auto probeKeys = [&] {
        const auto before = probe->mKeyProbes.load();
        send(WM_APP + 3, 0, 0);
        return probe->mKeyProbes.load() > before;
      };
      Check(probeKeys() && probe->mProbeShift == 0, "before: Shift isn't held for the app");
      const int capsBefore = probe->mProbeCapsLock.load();

      // H, as the injector sends it: Shift and Caps Lock (the opposite of
      // the real one) set, two characters queued, the key down and up
      const LPARAM keyDown = 0x00230001;// scan 0x23, repeat 1
      const LPARAM keyUp = keyDown | (LPARAM {1} << 30) | (LPARAM {1} << 31);
      const auto charsBefore = probe->mChars.load();
      begin();
      modifiers({.mShiftHeld = true, .mCapsLockOn = capsBefore != 1});
      character(L'H');
      character(L'i');
      const auto downs = probe->mKeyDowns.load();
      send(WM_KEYDOWN, 'H', keyDown);
      send(WM_KEYUP, 'H', keyUp);
      end();
      Check(probe->mKeyDowns.load() == downs + 1, "the app handled the key");
      Check(probe->mKeyShift == 1 && probe->mKeyStateShift == 1 && probe->mKeyAsyncShift == 1,
        std::format("Shift held for it: GetKeyState {}, GetKeyboardState {}, GetAsyncKeyState {}",
          probe->mKeyShift.load(), probe->mKeyStateShift.load(), probe->mKeyAsyncShift.load()));
      Check(probe->mKeyControl == 0, "Ctrl not held");
      Check(probe->mKeyCapsLock == (capsBefore == 1 ? 0 : 1), "Caps Lock as the keyboard set it");
      Check(probe->mKeyCharWaiting == L'H' && probe->mKeyCharLParamMatches == 1,
        std::format("while it handled the key-down, its character was already waiting, with the key's lParam ({})",
          probe->mKeyCharWaiting.load()));
      Check(WaitFor([&] { return probe->mChars.load() == charsBefore + 2; }, 2s), "then it got both characters");
      Check(probe->mCharCodes[charsBefore] == L'H' && probe->mCharCodes[charsBefore + 1] == L'i'
          && probe->mCharsMatchKey == 1,
        "in order (\"Hi\"), each with the key's lParam");
      Check(probeKeys() && probe->mProbeShift == 1, "within 100 ms of the end: Shift still held for it");
      std::this_thread::sleep_for(150ms);
      Check(probeKeys() && probe->mProbeShift == 0 && probe->mProbeCapsLock == capsBefore,
        "after that: its own key state back (Shift up, Caps Lock as it was)");

      // At most 16 characters wait
      const auto before16 = probe->mChars.load();
      begin();
      for (int i = 0; i < 20; ++i) {
        character(static_cast<wchar_t>(L'a' + i));
      }
      send(WM_KEYDOWN, 'A', 0x001E0001);
      end();
      Check(WaitFor([&] { return probe->mChars.load() >= before16 + Keys::CharacterQueueLimit; }, 2s),
        "20 queued: characters arrive");
      Pump(200ms);
      Check(probe->mChars.load() == before16 + Keys::CharacterQueueLimit,
        std::format("only {} of them ({})", Keys::CharacterQueueLimit, probe->mChars.load() - before16));

      // A character whose key never came isn't typed by the next injection's
      std::this_thread::sleep_for(150ms);
      const auto beforeStale = probe->mChars.load();
      begin();
      character(L'x');
      end();
      begin();
      send(WM_KEYDOWN, 'B', 0x00300001);
      end();
      Pump(200ms);
      Check(probe->mChars.load() == beforeStale, "a character whose key never came is dropped, not typed with the next key");

      // Everything released is a state like any other: it replaces the one
      // before, Caps Lock included (decisive when Caps Lock is really on)
      std::this_thread::sleep_for(150ms);
      begin();
      modifiers({.mShiftHeld = true, .mCtrlHeld = true, .mCapsLockOn = capsBefore != 1});
      modifiers({});
      send(WM_KEYDOWN, 'C', 0x002E0001);
      end();
      Check(probe->mKeyShift == 0 && probe->mKeyControl == 0 && probe->mKeyCapsLock == 0,
        capsBefore == 1 ? "everything released: Shift and Ctrl up, Caps Lock off though it's really on (a state, not the hand-back)"
                        : "everything released: Shift and Ctrl up, Caps Lock off");

      // The hand-back: the app's real key state at once, mid-injection
      std::this_thread::sleep_for(150ms);
      begin();
      modifiers({.mCtrlHeld = true, .mCapsLockOn = capsBefore != 1});
      keyCommand(Keys::KeyCommand::OverrideModifiers, Keys::HandBackKeys);
      send(WM_KEYDOWN, 'C', 0x002E0001);
      end();
      Check(probe->mKeyControl == 0 && probe->mKeyCapsLock == capsBefore,
        "the hand-back: its real key state at once (Ctrl up, Caps Lock as it really is)");

      // Arguments that decode to nothing (another build's, or junk) are
      // ignored: the state before stays, and no character is kept
      std::this_thread::sleep_for(150ms);
      const auto beforeJunk = probe->mChars.load();
      begin();
      modifiers({.mCtrlHeld = true});
      keyCommand(Keys::KeyCommand::OverrideModifiers, 0x1);
      keyCommand(Keys::KeyCommand::CharacterForNextKey, 0x10041);
      send(WM_KEYDOWN, 'C', 0x002E0001);
      end();
      Check(probe->mKeyControl == 1, "a modifier argument with a stray bit: ignored (Ctrl still held as set before it)");
      Pump(200ms);
      Check(probe->mChars.load() == beforeJunk, "a character argument wider than one UTF-16 unit: ignored");

      // Outside an injection, a key-down gets no queued characters
      std::this_thread::sleep_for(150ms);
      const auto beforeOutside = probe->mChars.load();
      send(WM_KEYDOWN, 'D', 0x00200001);
      Pump(200ms);
      Check(probe->mChars.load() == beforeOutside, "a key-down outside an injection: nothing typed");
    });

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
