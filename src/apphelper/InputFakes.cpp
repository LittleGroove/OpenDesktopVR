// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "InputFakes.hpp"

#include <OpenDesktopVR/AppHelperProtocol.hpp>
#include <OpenDesktopVR/ImportPatch.hpp>

#include <array>
#include <optional>

namespace OpenDesktopVR::AppHelper::InputFakes {

std::atomic<int> gInFlight {0};

namespace {

namespace Protocol = AppHelperProtocol;

// ---- Per-thread injection windows (HK-5) ----------------------------------
// A few UI threads at most; a fixed table, no thread-local storage in
// someone else's app
constexpr std::size_t MaxThreads = 8;

struct Window {
  std::atomic<DWORD> mThread {0};
  std::atomic<int> mNesting {0};
  std::atomic<ULONGLONG> mExpireAt {0};
  std::atomic<ULONGLONG> mGraceUntil {0};
  std::atomic<LONG> mX {0};
  std::atomic<LONG> mY {0};
  std::atomic<HWND> mTopLevel {nullptr};
  std::atomic<LONG> mButtons {0};// MK_LBUTTON ...
};
std::array<Window, MaxThreads> gWindows {};
// Any window open or in its grace: the fakes' quick way out
std::atomic<int> gOpen {0};

Window* Find(DWORD thread) noexcept {
  for (auto& window: gWindows) {
    if (window.mThread.load(std::memory_order_acquire) == thread) {
      return &window;
    }
  }
  return nullptr;
}

Window* FindOrClaim(DWORD thread) noexcept {
  if (auto* found = Find(thread)) {
    return found;
  }
  for (auto& window: gWindows) {
    DWORD free = 0;
    if (window.mThread.compare_exchange_strong(free, thread, std::memory_order_acq_rel)) {
      return &window;
    }
  }
  return nullptr;
}

bool CursorOpen(const Window& window, ULONGLONG now) noexcept {
  return window.mNesting.load(std::memory_order_acquire) > 0 && now < window.mExpireAt.load(std::memory_order_acquire);
}

bool FocusOpen(const Window& window, ULONGLONG now) noexcept {
  return CursorOpen(window, now) || now < window.mGraceUntil.load(std::memory_order_acquire);
}

// This thread's window, if any is open (the common case costs one load)
const Window* Mine() noexcept {
  if (gOpen.load(std::memory_order_acquire) == 0) {
    return nullptr;
  }
  return Find(GetCurrentThreadId());
}

struct InFlight {
  InFlight() noexcept {
    gInFlight.fetch_add(1, std::memory_order_acquire);
  }
  ~InFlight() {
    gInFlight.fetch_sub(1, std::memory_order_release);
  }
  InFlight(const InFlight&) = delete;
  InFlight& operator=(const InFlight&) = delete;
};

// ---- The real functions, and the fakes -------------------------------------
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using GetMessagePosFn = DWORD(WINAPI*)();
using GetForegroundWindowFn = HWND(WINAPI*)();
using SetForegroundWindowFn = BOOL(WINAPI*)(HWND);
using BringWindowToTopFn = BOOL(WINAPI*)(HWND);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SendInputFn = UINT(WINAPI*)(UINT, LPINPUT, int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using WindowFromPointFn = HWND(WINAPI*)(POINT);
using TrackMouseEventFn = BOOL(WINAPI*)(LPTRACKMOUSEEVENT);
using GetMessageExtraInfoFn = LPARAM(WINAPI*)();

GetCursorPosFn gGetCursorPos {nullptr};
GetMessagePosFn gGetMessagePos {nullptr};
GetForegroundWindowFn gGetForegroundWindow {nullptr};
SetForegroundWindowFn gSetForegroundWindow {nullptr};
BringWindowToTopFn gBringWindowToTop {nullptr};
SetWindowPosFn gSetWindowPos {nullptr};
SendInputFn gSendInput {nullptr};
GetKeyStateFn gGetKeyState {nullptr};
GetKeyStateFn gGetAsyncKeyState {nullptr};
WindowFromPointFn gWindowFromPoint {nullptr};
TrackMouseEventFn gTrackMouseEvent {nullptr};
GetMessageExtraInfoFn gGetMessageExtraInfo {nullptr};

// The finger's buttons, for a mouse button key code; nullopt for other keys
std::optional<bool> FingerButton(const Window& window, int key) noexcept {
  const LONG buttons = window.mButtons.load(std::memory_order_relaxed);
  switch (key) {
    case VK_LBUTTON:
      return (buttons & MK_LBUTTON) != 0;
    case VK_RBUTTON:
      return (buttons & MK_RBUTTON) != 0;
    case VK_MBUTTON:
      return (buttons & MK_MBUTTON) != 0;
    default:
      return std::nullopt;
  }
}

BOOL WINAPI FakeGetCursorPos(LPPOINT point) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && point && CursorOpen(*window, GetTickCount64())) {
    point->x = window->mX.load(std::memory_order_relaxed);
    point->y = window->mY.load(std::memory_order_relaxed);
    return TRUE;
  }
  return gGetCursorPos(point);
}

DWORD WINAPI FakeGetMessagePos() {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && CursorOpen(*window, GetTickCount64())) {
    return static_cast<DWORD>(
      Protocol::PackPoint(window->mX.load(std::memory_order_relaxed), window->mY.load(std::memory_order_relaxed)));
  }
  return gGetMessagePos();
}

HWND WINAPI FakeGetForegroundWindow() {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && FocusOpen(*window, GetTickCount64())) {
    if (const HWND topLevel = window->mTopLevel.load(std::memory_order_relaxed)) {
      return topLevel;
    }
  }
  return gGetForegroundWindow();
}

// HK-8: the app can't bring itself over the game while we inject
BOOL WINAPI FakeSetForegroundWindow(HWND target) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && FocusOpen(*window, GetTickCount64())) {
    return TRUE;
  }
  return gSetForegroundWindow(target);
}

BOOL WINAPI FakeBringWindowToTop(HWND target) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && FocusOpen(*window, GetTickCount64())) {
    return TRUE;
  }
  return gBringWindowToTop(target);
}

BOOL WINAPI FakeSetWindowPos(HWND target, HWND after, int x, int y, int cx, int cy, UINT flags) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && FocusOpen(*window, GetTickCount64())) {
    // Moves and sizes go through; z-order and activation don't
    flags |= SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
  }
  return gSetWindowPos(target, after, x, y, cx, cy, flags);
}

SHORT WINAPI FakeGetKeyState(int key) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && CursorOpen(*window, GetTickCount64())) {
    if (const auto held = FingerButton(*window, key)) {
      return *held ? static_cast<SHORT>(0x8000) : 0;
    }
  }
  return gGetKeyState(key);
}

SHORT WINAPI FakeGetAsyncKeyState(int key) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && CursorOpen(*window, GetTickCount64())) {
    if (const auto held = FingerButton(*window, key)) {
      return *held ? static_cast<SHORT>(0x8000) : 0;
    }
  }
  return gGetAsyncKeyState(key);
}

// The app's own window at the injected point, not the game in front of it
HWND WINAPI FakeWindowFromPoint(POINT point) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && CursorOpen(*window, GetTickCount64())) {
    const HWND topLevel = window->mTopLevel.load(std::memory_order_relaxed);
    RECT bounds {};
    if (topLevel && GetWindowRect(topLevel, &bounds) && PtInRect(&bounds, point)) {
      HWND found = topLevel;
      POINT local = point;
      ScreenToClient(topLevel, &local);
      for (int depth = 0; depth < 32; ++depth) {
        const HWND child = ChildWindowFromPointEx(found, local, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!child || child == found) {
          break;
        }
        MapWindowPoints(found, child, &local, 1);
        found = child;
      }
      return found;
    }
  }
  return gWindowFromPoint(point);
}

// The real cursor is over the game: tracking it would report a leave at
// once. The injector sends the leave when the finger goes.
BOOL WINAPI FakeTrackMouseEvent(LPTRACKMOUSEEVENT track) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && track && CursorOpen(*window, GetTickCount64())
      && (track->dwFlags & TME_QUERY) == 0) {
    return TRUE;
  }
  return gTrackMouseEvent(track);
}

// A plain mouse message: not one Windows made from touch or a pen
LPARAM WINAPI FakeGetMessageExtraInfo() {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && CursorOpen(*window, GetTickCount64())) {
    return 0;
  }
  return gGetMessageExtraInfo();
}

// HK-8: what the app sends would go to the game, the real foreground
UINT WINAPI FakeSendInput(UINT count, LPINPUT inputs, int size) {
  const InFlight inFlight;
  if (const auto* window = Mine(); window && FocusOpen(*window, GetTickCount64())) {
    return count;// as if sent
  }
  return gSendInput(count, inputs, size);
}

struct Fake {
  const char* mName;
  void** mReal;
  void* mFake;
  ImportPatch mPatch;
};

std::array<Fake, 12> gFakes {{
  {"GetCursorPos", reinterpret_cast<void**>(&gGetCursorPos), reinterpret_cast<void*>(&FakeGetCursorPos), {}},
  {"GetMessagePos", reinterpret_cast<void**>(&gGetMessagePos), reinterpret_cast<void*>(&FakeGetMessagePos), {}},
  {"GetForegroundWindow", reinterpret_cast<void**>(&gGetForegroundWindow), reinterpret_cast<void*>(&FakeGetForegroundWindow), {}},
  {"SetForegroundWindow", reinterpret_cast<void**>(&gSetForegroundWindow), reinterpret_cast<void*>(&FakeSetForegroundWindow), {}},
  {"BringWindowToTop", reinterpret_cast<void**>(&gBringWindowToTop), reinterpret_cast<void*>(&FakeBringWindowToTop), {}},
  {"SetWindowPos", reinterpret_cast<void**>(&gSetWindowPos), reinterpret_cast<void*>(&FakeSetWindowPos), {}},
  {"SendInput", reinterpret_cast<void**>(&gSendInput), reinterpret_cast<void*>(&FakeSendInput), {}},
  {"GetKeyState", reinterpret_cast<void**>(&gGetKeyState), reinterpret_cast<void*>(&FakeGetKeyState), {}},
  {"GetAsyncKeyState", reinterpret_cast<void**>(&gGetAsyncKeyState), reinterpret_cast<void*>(&FakeGetAsyncKeyState), {}},
  {"WindowFromPoint", reinterpret_cast<void**>(&gWindowFromPoint), reinterpret_cast<void*>(&FakeWindowFromPoint), {}},
  {"TrackMouseEvent", reinterpret_cast<void**>(&gTrackMouseEvent), reinterpret_cast<void*>(&FakeTrackMouseEvent), {}},
  {"GetMessageExtraInfo", reinterpret_cast<void**>(&gGetMessageExtraInfo), reinterpret_cast<void*>(&FakeGetMessageExtraInfo), {}},
}};

bool LoadReal() noexcept {
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    return false;
  }
  for (auto& fake: gFakes) {
    if (!*fake.mReal) {
      *fake.mReal = reinterpret_cast<void*>(GetProcAddress(user32, fake.mName));
    }
    if (!*fake.mReal) {
      return false;
    }
  }
  return true;
}

}// namespace

std::size_t Apply(HMODULE skip) noexcept {
  if (!LoadReal()) {
    return 0;
  }
  std::size_t slots = 0;
  for (auto& fake: gFakes) {
    slots += fake.mPatch.Apply(fake.mName, *fake.mReal, fake.mFake, skip);
  }
  return slots;
}

void Restore() noexcept {
  for (auto& window: gWindows) {
    window.mNesting = 0;
    window.mGraceUntil = 0;
    window.mButtons = 0;
  }
  gOpen = 0;
  for (auto& fake: gFakes) {
    fake.mPatch.Restore();
  }
}

std::size_t Count() noexcept {
  std::size_t slots = 0;
  for (const auto& fake: gFakes) {
    slots += fake.mPatch.Count();
  }
  return slots;
}

void Begin(HWND target, LPARAM packedPoint) noexcept {
  auto* window = FindOrClaim(GetCurrentThreadId());
  if (!window) {
    return;
  }
  const auto now = GetTickCount64();
  // A begin after a window that expired (its end lost) starts fresh
  if (now >= window->mExpireAt.load() && window->mNesting.load() > 0) {
    window->mNesting = 0;
  }
  const auto point = Protocol::UnpackPoint(packedPoint);
  window->mX = point.x;
  window->mY = point.y;
  window->mTopLevel = target ? GetAncestor(target, GA_ROOT) : nullptr;
  window->mExpireAt = now + ExpireMs;
  window->mGraceUntil = now + ExpireMs;// focus lasts as long as the window
  window->mNesting.fetch_add(1);
  gOpen = 1;
}

void SetMouseButtons(LPARAM buttons) noexcept {
  if (auto* window = FindOrClaim(GetCurrentThreadId())) {
    window->mButtons = static_cast<LONG>(buttons);
  }
}

void End() noexcept {
  auto* window = Find(GetCurrentThreadId());
  if (!window) {
    return;
  }
  // Never below zero
  int nesting = window->mNesting.load();
  while (nesting > 0 && !window->mNesting.compare_exchange_weak(nesting, nesting - 1)) {
  }
  if (nesting <= 1) {
    window->mGraceUntil = GetTickCount64() + GraceMs;
  }
}

void Activate(HWND target) noexcept {
  const HWND topLevel = target ? GetAncestor(target, GA_ROOT) : nullptr;
  if (!topLevel || GetWindowThreadProcessId(topLevel, nullptr) != GetCurrentThreadId() || GetActiveWindow() == topLevel) {
    return;
  }
  // Meant for this thread only. But Windows lets the app take the real
  // foreground when it may (it was started by the foreground process, or
  // nobody has touched the keyboard or mouse for a while): if it did, it's
  // given straight back (R1). These calls are the helper's own, never faked.
  const HWND foreground = GetForegroundWindow();
  SetActiveWindow(topLevel);
  if (foreground && foreground != topLevel && GetForegroundWindow() == topLevel) {
    SetForegroundWindow(foreground);
  }
}

bool CursorFaked() noexcept {
  const auto* window = Mine();
  return window && CursorOpen(*window, GetTickCount64());
}

bool FocusFaked() noexcept {
  const auto* window = Mine();
  return window && FocusOpen(*window, GetTickCount64());
}

}// namespace OpenDesktopVR::AppHelper::InputFakes
