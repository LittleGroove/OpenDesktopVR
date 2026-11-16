// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The app helper DLL (doc 06; ADR-031): loaded into a captured app by the
// host's WH_GETMESSAGE hook on the captured window's UI thread. So far it does
// one thing: it keeps the app painting while the game covers it (CT-3).
//
// Chromium-based apps (Edge, Chrome, Discord, Spotify) and Firefox check
// whether other windows cover theirs, and stop painting about a second after
// they're fully covered. While a captured window is on here, the app's own
// modules see other processes' top-level windows as hidden when they ask
// IsWindowVisible (HK-6), so nothing covers it.
//
// Rules for code that runs inside someone else's app:
// - the visibility check runs constantly on the app's threads: it takes no
//   lock, allocates nothing, and only reads atomics;
// - commands run on the hooked UI thread under one lock; nothing allocates
//   (fixed-size state); nothing throws out of the hook; nothing waits on the
//   host; nothing is logged (the host logs what the replies say).

#include <OpenDesktopVR/AppHelperProtocol.hpp>
#include <OpenDesktopVR/ImportPatch.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>

namespace {

namespace Protocol = OpenDesktopVR::AppHelperProtocol;
using OpenDesktopVR::AppHelper::ImportPatch;

constexpr std::size_t MaxWindows = 16;
// HK-9: how long Unload waits for calls already in the visibility check
constexpr ULONGLONG InFlightWaitMs = 200;

using IsWindowVisibleFn = BOOL(WINAPI*)(HWND);

HMODULE gModule {nullptr};

// ---- Read by the visibility check, on any thread --------------------------
std::atomic<int> gWindowsOn {0};
std::atomic<int> gInFlight {0};
IsWindowVisibleFn gRealIsWindowVisible {nullptr};// set once, before any patch
DWORD gOwnProcess {0};
HWND gDesktop {nullptr};

// ---- Commands only (under gLock) ------------------------------------------
SRWLOCK gLock = SRWLOCK_INIT;
std::array<HWND, MaxWindows> gWindows {};
ImportPatch gPatch;
std::atomic<HWND> gReplyWindow {nullptr};
std::atomic<bool> gRetired {false};// pinned after an unload that couldn't finish

std::atomic<UINT> gCommandMessage {0};
std::atomic<UINT> gReplyMessage {0};

// Registered once (the same ID every time), without a function-local static
UINT Registered(std::atomic<UINT>& cache, const wchar_t* name) noexcept {
  UINT message = cache.load(std::memory_order_relaxed);
  if (message == 0) {
    message = RegisterWindowMessageW(name);
    cache.store(message, std::memory_order_relaxed);
  }
  return message;
}

UINT CommandMessage() noexcept {
  return Registered(gCommandMessage, Protocol::CommandMessageName);
}

UINT ReplyMessage() noexcept {
  return Registered(gReplyMessage, Protocol::ReplyMessageName);
}

// HK-6: only other processes' top-level windows, never the desktop window,
// never the app's own (forcing its own windows visible once broke them)
bool LooksHidden(HWND window) noexcept {
  if (!window || window == gDesktop || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0) {
    return false;
  }
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  return process != 0 && process != gOwnProcess;
}

BOOL WINAPI KeepPaintingIsWindowVisible(HWND window) {
  gInFlight.fetch_add(1, std::memory_order_acquire);
  const BOOL visible = gWindowsOn.load(std::memory_order_acquire) > 0 && LooksHidden(window)
    ? FALSE
    : gRealIsWindowVisible(window);
  gInFlight.fetch_sub(1, std::memory_order_release);
  return visible;
}

void Reply(Protocol::Command command, Protocol::Result result, std::size_t detail = 0) noexcept {
  if (const HWND window = gReplyWindow.load()) {
    const Protocol::Reply reply {command, result, static_cast<uint16_t>(std::min<std::size_t>(detail, 0xFFFF))};
    PostMessageW(window, ReplyMessage(), Protocol::Pack(reply), static_cast<LPARAM>(GetCurrentThreadId()));
  }
}

// HK-7: the app re-checks what covers it on window events; one for this
// window makes it look again now, so an app already marked hidden recovers
void LookAgain(HWND window) noexcept {
  NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, window, OBJID_WINDOW, CHILDID_SELF);
}

std::size_t CountOn() noexcept {
  std::size_t count = 0;
  for (const auto window: gWindows) {
    count += window ? 1 : 0;
  }
  return count;
}

void KeepPaintingOn(HWND window) noexcept {
  if (!IsWindow(window)) {
    Reply(Protocol::Command::KeepPaintingOn, Protocol::Result::Failed);
    return;
  }
  if (!gRealIsWindowVisible) {
    gRealIsWindowVisible = reinterpret_cast<IsWindowVisibleFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowVisible")));
    gOwnProcess = GetCurrentProcessId();
    gDesktop = GetDesktopWindow();
  }
  if (!gRealIsWindowVisible) {
    Reply(Protocol::Command::KeepPaintingOn, Protocol::Result::Failed);
    return;
  }
  // Once per window (a repeated command changes nothing)
  bool listed = false;
  for (const auto on: gWindows) {
    listed |= on == window;
  }
  if (!listed) {
    auto free = std::find(gWindows.begin(), gWindows.end(), nullptr);
    if (free == gWindows.end()) {
      Reply(Protocol::Command::KeepPaintingOn, Protocol::Result::TooManyWindows);
      return;
    }
    *free = window;
  }
  // Modules loaded since the last command are patched too
  const auto slots = gPatch.Apply("IsWindowVisible", reinterpret_cast<void*>(gRealIsWindowVisible),
    reinterpret_cast<void*>(&KeepPaintingIsWindowVisible), gModule);
  gWindowsOn.store(static_cast<int>(CountOn()), std::memory_order_release);
  LookAgain(window);
  Reply(Protocol::Command::KeepPaintingOn, slots > 0 ? Protocol::Result::Ok : Protocol::Result::NothingToPatch, slots);
}

void KeepPaintingOff(HWND window) noexcept {
  for (auto& on: gWindows) {
    if (on == window) {
      on = nullptr;
    }
  }
  gWindowsOn.store(static_cast<int>(CountOn()), std::memory_order_release);
  if (IsWindow(window)) {
    LookAgain(window);
  }
  Reply(Protocol::Command::KeepPaintingOff, Protocol::Result::Ok);
}

// HK-9: everything undone, and nothing left running in this DLL, before the
// host removes the hook (which unloads it)
void Unload() noexcept {
  gWindows.fill(nullptr);
  gWindowsOn.store(0, std::memory_order_release);
  gPatch.Restore();
  // A thread may have read a patched slot just before it was put back and
  // not be inside yet: a moment for it, then wait for those inside
  Sleep(2);
  const ULONGLONG until = GetTickCount64() + InFlightWaitMs;
  while (gInFlight.load(std::memory_order_acquire) > 0 && GetTickCount64() < until) {
    Sleep(1);
  }
  const int left = gInFlight.load(std::memory_order_acquire);
  if (left > 0) {
    // Never unloaded under a running call: stay loaded, doing nothing
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      reinterpret_cast<LPCWSTR>(&KeepPaintingIsWindowVisible), &pinned);
    gRetired = true;
  }
  Reply(Protocol::Command::Unload, left > 0 ? Protocol::Result::StillBusy : Protocol::Result::Ok, static_cast<std::size_t>(left));
}

void Handle(WPARAM command, LPARAM argument) noexcept {
  AcquireSRWLockExclusive(&gLock);
  switch (static_cast<Protocol::Command>(command)) {
    case Protocol::Command::Hello:
      gReplyWindow = reinterpret_cast<HWND>(argument);
      Reply(Protocol::Command::Hello, gRetired ? Protocol::Result::Failed : Protocol::Result::Ok, Protocol::Version);
      break;
    case Protocol::Command::KeepPaintingOn:
      if (gRetired) {
        Reply(Protocol::Command::KeepPaintingOn, Protocol::Result::Failed);
      } else {
        KeepPaintingOn(reinterpret_cast<HWND>(argument));
      }
      break;
    case Protocol::Command::KeepPaintingOff:
      KeepPaintingOff(reinterpret_cast<HWND>(argument));
      break;
    case Protocol::Command::Unload:
      Unload();
      break;
    default:
      break;// another build's command, or a keyboard one: not handled here yet
  }
  ReleaseSRWLockExclusive(&gLock);
}

}// namespace

// The hook procedure. Called on the hooked thread for every message it
// takes from its queue, so the common case is one comparison.
extern "C" __declspec(dllexport) LRESULT CALLBACK OdvrAppHelperGetMessageProc(int code, WPARAM wParam, LPARAM lParam) {
  // HK-3: negative codes pass straight through, and only removed messages
  // count (a message only peeked at comes back again)
  if (code == HC_ACTION && wParam == PM_REMOVE && lParam != 0) {
    auto* message = reinterpret_cast<MSG*>(lParam);
    if (message->message == CommandMessage() && message->message != 0) {
      const WPARAM command = message->wParam;
      const LPARAM argument = message->lParam;
      // The app never sees it
      message->message = WM_NULL;
      message->wParam = 0;
      message->lParam = 0;
      Handle(command, argument);
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    gModule = module;
    DisableThreadLibraryCalls(module);
  } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
    // Unloaded without Unload (the host stopped, or gave up waiting): put
    // the app's imports back before this code goes away. Without the lock:
    // a command may be stuck, and the restore is a compare-exchange per slot.
    gWindowsOn.store(0, std::memory_order_release);
    if (gPatch.Count() > 0) {
      gPatch.Restore();
      const ULONGLONG until = GetTickCount64() + InFlightWaitMs;
      while (gInFlight.load(std::memory_order_acquire) > 0 && GetTickCount64() < until) {
        Sleep(1);
      }
    }
  }
  return TRUE;
}
