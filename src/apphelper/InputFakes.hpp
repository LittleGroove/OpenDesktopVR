// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The helper's input mode (doc 06 "Why a helper is needed", "Helper
// behavior requirements"; HK-5, HK-8, HK-9; M4b). Inside the captured app.
//
// The host's injector brackets each event it sends with BeginInjection and
// EndInjection (sent, so they arrive in order with the event). Between them,
// on that thread only (HK-5):
// - the app sees the cursor at the event's point (GetCursorPos,
//   GetMessagePos), its own window under it (WindowFromPoint), the mouse
//   buttons as the finger holds them (GetKeyState, GetAsyncKeyState), a
//   plain mouse message (GetMessageExtraInfo), and asking to hear when the
//   mouse leaves succeeds without Windows saying at once that it has (the
//   real cursor is elsewhere; TrackMouseEvent): only while an injected
//   message is being handled;
// - its window is the foreground window (GetForegroundWindow), it can't
//   change its windows' z-order or bring itself forward (SetWindowPos,
//   BringWindowToTop, SetForegroundWindow), and its own SendInput goes
//   nowhere (it would reach the game): these last until 100 ms after the
//   end (HK-8);
// - an end that never comes closes the window after 2 s; the nesting count
//   never goes below zero; a begin after an expired window starts fresh;
// - if the app took the real foreground meanwhile (Windows lets some apps
//   do so when they activate on a click), the end gives it back to the
//   window that had it when the injection began (R1).
//
// For keys (M5a), the keyboard's modifiers on the app's thread and the
// characters queued with each key-down: see "Keys" below.
//
// The fakes are import-table redirections (ImportPatch), set up by InputOn
// at capture start (HK-1) and put back by Unload. They read atomics only,
// take no lock and allocate nothing; each counts itself in flight so
// Unload can wait for it (HK-9).
#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>

namespace OpenDesktopVR::AppHelper::InputFakes {

// How long after EndInjection the focus fakes last, and when a window whose
// end never came closes
inline constexpr ULONGLONG GraceMs = 100;
inline constexpr ULONGLONG ExpireMs = 2000;

// The calls in flight in any fake (with the visibility check's, for Unload)
extern std::atomic<int> gInFlight;

// Sets up the fakes (once their real functions are known). Returns how many
// import slots point at the helper, across all the fakes. `skip`: the
// helper's own module. Under the helper's command lock.
std::size_t Apply(HMODULE skip) noexcept;
// Puts every import back
void Restore() noexcept;
// Import slots redirected now
std::size_t Count() noexcept;

// The sent commands, on the hooked thread (the WH_CALLWNDPROC hook)
void Begin(HWND window, LPARAM packedPoint) noexcept;
void End() noexcept;
void SetMouseButtons(LPARAM buttons) noexcept;
// Makes `window` the active window on this thread only, if it isn't; if
// that took the real foreground, gives it back
void Activate(HWND window) noexcept;

// ---- Keys (M5a; the keyboard commands of InjectionHookProtocol) ----------
// Doc 06: the app's thread sees Shift, Ctrl, Alt and the Caps and Num Lock
// toggles as the virtual keyboard says. The thread's own key state is kept
// and replaced (SetKeyboardState, so GetKeyState and GetKeyboardState see
// it); GetAsyncKeyState is faked for the modifiers. The thread's own state
// comes back on its first message after the injection window closes (or at
// once for 0).
void SetKeyboardModifiers(LPARAM bits) noexcept;
// A character to type with the next key that goes down in this injection
// (at most 16 waiting; a new injection starts with none)
void QueueChar(LPARAM character) noexcept;
// An injected key-down sent to `window` on this thread, before the app
// handles it: the waiting characters are posted to it (WM_CHAR, with the
// key's lParam), so they're in the queue while the app handles the key, as
// TranslateMessage would have put them (INJ-9)
void KeyDown(HWND window, LPARAM keyLParam) noexcept;
// On every message the thread handles: its own key state back, once its
// injection window has closed. One load when nothing is waiting.
void RestoreKeysIfDone() noexcept;
// Quick checks for the hooks: one load each
bool CharsWaiting() noexcept;
bool KeysReplaced() noexcept;

// For the helper's tests: whether this thread is inside an injected
// message, and inside the focus window
bool CursorFaked() noexcept;
bool FocusFaked() noexcept;

}// namespace OpenDesktopVR::AppHelper::InputFakes
