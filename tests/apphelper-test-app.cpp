// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The app apphelper-test loads the app helper into. Like a Chromium app:
// - its windows' class starts with Chrome_WidgetWin_;
// - a background thread keeps asking IsWindowVisible about other windows,
//   through its own import table and through a DLL that delay-loads user32;
// - a busy thread asks as fast as it can (unloading under load, HK-9);
// - its message loop peeks at every message before taking it (HK-3);
// - when its main window gets a click (input mode, M4b), it records where it
//   thinks the cursor is, what's in the foreground and active, and what its
//   probe thread sees; then it tries to come to the top and to SendInput, as
//   apps do.
// It writes what it sees to the test's shared memory (apphelper-probe.hpp).

#include "apphelper-probe.hpp"

#include <Windows.h>

#include <thread>

using OpenDesktopVR::Testing::AppHelperProbe;

extern "C" __declspec(dllimport) BOOL ProbeIsWindowVisible(HWND window);

namespace {

AppHelperProbe* gProbe {nullptr};
HWND gMain {nullptr};
HWND gSecond {nullptr};

// Whether `upper` is above `lower` in the z-order
bool Above(HWND upper, HWND lower) {
  for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
    if (window == upper) {
      return true;
    }
    if (window == lower) {
      return false;
    }
  }
  return false;
}
// The probe thread checks the fakes from outside the UI thread on request
std::atomic<int> gAskOtherThread {0};
HANDLE gOtherThreadAnswered {nullptr};

LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_LBUTTONDOWN && window == gMain) {
    POINT cursor {};
    GetCursorPos(&cursor);
    gProbe->mDownCursorX = cursor.x;
    gProbe->mDownCursorY = cursor.y;
    gProbe->mDownMessagePos = GetMessagePos();
    gProbe->mDownForeground = GetForegroundWindow() == gMain ? 1 : 0;
    gProbe->mDownActive = GetActiveWindow() == gMain ? 1 : 0;
    // Another thread, at this very moment (HK-5)
    ResetEvent(gOtherThreadAnswered);
    gAskOtherThread = 1;
    WaitForSingleObject(gOtherThreadAnswered, 1000);
    // As apps do on a click: come to the top, and SendInput (HK-8)
    SetWindowPos(gMain, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    BringWindowToTop(gMain);
    gProbe->mMainCameUp = Above(gMain, gSecond) ? 1 : 0;
    // One pixel right and back: the cursor ends where it was
    INPUT inputs[2] {};
    inputs[0].type = inputs[1].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = inputs[1].mi.dwFlags = MOUSEEVENTF_MOVE;
    inputs[0].mi.dx = 1;
    inputs[1].mi.dx = -1;
    SendInput(2, inputs, sizeof(INPUT));
    ++gProbe->mDowns;
    return 0;
  }
  if (message == WM_APP + 2 && window == gMain) {
    SetWindowPos(gSecond, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    gProbe->mSecondOnTop = Above(gSecond, gMain) ? 1 : 0;
    return 0;
  }
  if (message == WM_APP + 1 && window == gMain) {
    gProbe->mProbeForeground = GetForegroundWindow() == gMain ? 1 : 0;
    ++gProbe->mProbes;
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
  if (window == gMain && object == OBJID_WINDOW && child == CHILDID_SELF) {
    ++gProbe->mLocationChanges;
  }
}

HWND MakeWindow(const wchar_t* title, int x, bool visible) {
  return CreateWindowExW(0, L"Chrome_WidgetWin_OdvrTest", title, WS_OVERLAPPEDWINDOW | (visible ? WS_VISIBLE : 0), x, 0,
    220, 140, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

}// namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, OpenDesktopVR::Testing::AppHelperProbeName);
  if (!mapping) {
    return 1;
  }
  gProbe = static_cast<AppHelperProbe*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AppHelperProbe)));
  if (!gProbe) {
    return 2;
  }
  WNDCLASSW windowClass {};
  windowClass.lpfnWndProc = &MainWindowProc;
  windowClass.hInstance = instance;
  windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
  windowClass.lpszClassName = L"Chrome_WidgetWin_OdvrTest";
  RegisterClassW(&windowClass);
  gMain = MakeWindow(L"apphelper-test app", 0, true);
  const HWND second = MakeWindow(L"apphelper-test app 2", 230, true);
  gSecond = second;
  const HWND hidden = MakeWindow(L"apphelper-test hidden", 460, false);
  const HWINEVENTHOOK events = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
    &OnWinEvent, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);

  const auto cover = reinterpret_cast<HWND>(gProbe->mCover);
  const auto coverChild = reinterpret_cast<HWND>(gProbe->mCoverChild);
  gOtherThreadAnswered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread probe([&] {
    while (gProbe->mStop == 0) {
      if (gAskOtherThread.exchange(0) != 0) {
        POINT cursor {};
        GetCursorPos(&cursor);
        gProbe->mOtherThreadCursorFaked = (cursor.x == gProbe->mDownCursorX && cursor.y == gProbe->mDownCursorY) ? 1 : 0;
        gProbe->mOtherThreadForegroundFaked = GetForegroundWindow() == gMain ? 1 : 0;
        SetEvent(gOtherThreadAnswered);
      }
      gProbe->mCoverVisible = IsWindowVisible(cover) ? 1 : 0;
      gProbe->mCoverVisibleDelayLoaded = ProbeIsWindowVisible(cover) ? 1 : 0;
      gProbe->mCoverChildVisible = IsWindowVisible(coverChild) ? 1 : 0;
      gProbe->mDesktopVisible = IsWindowVisible(GetDesktopWindow()) ? 1 : 0;
      gProbe->mOwnVisible = IsWindowVisible(gMain) ? 1 : 0;
      gProbe->mOwnHidden = IsWindowVisible(hidden) ? 1 : 0;
      gProbe->mHelperLoaded = GetModuleHandleW(L"OpenDesktopVR-AppHelper.dll") ? 1 : 0;
      ++gProbe->mRounds;
      Sleep(10);
    }
  });
  std::thread busy([&] {
    while (gProbe->mStop == 0) {
      IsWindowVisible(cover);
      ++gProbe->mBusyCalls;
    }
  });
  gProbe->mMainWindow = reinterpret_cast<uint64_t>(gMain);
  gProbe->mSecondWindow = reinterpret_cast<uint64_t>(second);

  while (gProbe->mStop == 0) {
    MSG message {};
    // Looked at before it's taken, like many apps' loops (HK-3)
    if (PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE)) {
      if (GetMessageW(&message, nullptr, 0, 0) <= 0) {
        break;
      }
      DispatchMessageW(&message);
    } else {
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
  }
  gProbe->mStop = 1;
  probe.join();
  busy.join();
  UnhookWinEvent(events);
  CloseHandle(gOtherThreadAnswered);
  DestroyWindow(hidden);
  DestroyWindow(second);
  DestroyWindow(gMain);
  UnmapViewOfFile(gProbe);
  CloseHandle(mapping);
  return 0;
}
