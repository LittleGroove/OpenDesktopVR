// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The app apphelper-test loads the app helper into. Like a Chromium app:
// - its windows' class starts with Chrome_WidgetWin_;
// - a background thread keeps asking IsWindowVisible about other windows,
//   through its own import table and through a DLL that delay-loads user32;
// - a busy thread asks as fast as it can (unloading under load, HK-9);
// - its message loop peeks at every message before taking it (HK-3).
// It writes what it sees to the test's shared memory (apphelper-probe.hpp).

#include "apphelper-probe.hpp"

#include <Windows.h>

#include <thread>

using OpenDesktopVR::Testing::AppHelperProbe;

extern "C" __declspec(dllimport) BOOL ProbeIsWindowVisible(HWND window);

namespace {

AppHelperProbe* gProbe {nullptr};
HWND gMain {nullptr};

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
  windowClass.lpfnWndProc = &DefWindowProcW;
  windowClass.hInstance = instance;
  windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
  windowClass.lpszClassName = L"Chrome_WidgetWin_OdvrTest";
  RegisterClassW(&windowClass);
  gMain = MakeWindow(L"apphelper-test app", 0, true);
  const HWND second = MakeWindow(L"apphelper-test app 2", 230, true);
  const HWND hidden = MakeWindow(L"apphelper-test hidden", 460, false);
  const HWINEVENTHOOK events = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
    &OnWinEvent, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);

  const auto cover = reinterpret_cast<HWND>(gProbe->mCover);
  const auto coverChild = reinterpret_cast<HWND>(gProbe->mCoverChild);
  std::thread probe([&] {
    while (gProbe->mStop == 0) {
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
  DestroyWindow(hidden);
  DestroyWindow(second);
  DestroyWindow(gMain);
  UnmapViewOfFile(gProbe);
  CloseHandle(mapping);
  return 0;
}
