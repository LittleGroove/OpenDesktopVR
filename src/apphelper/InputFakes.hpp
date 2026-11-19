// SPDX-License-Identifier: GPL-3.0-only
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
//   real cursor is elsewhere; TrackMouseEvent). These hold from the begin to
//   the end, and not in the grace after it;
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

// ---- Keys (M5a; InjectionHookProtocol::KeyCommand) ------------------------
// OverrideModifiers: a copy of this thread's real keyboard state is taken,
// and the virtual keyboard's held modifiers and lock toggles (doc 06) are
// written over it with SetKeyboardState, which GetKeyState and
// GetKeyboardState then report; the GetAsyncKeyState fake reports the held
// ones too. The override stays until the injection window and its grace
// have closed, and the copy goes back on the thread's next message;
// HandBackKeys puts the copy back immediately. An argument that decodes to
// no state is ignored.
void OverrideModifiers(LPARAM argument) noexcept;
// CharacterForNextKey: one UTF-16 unit kept back for the next injected
// key-down on this thread. Up to CharacterQueueLimit are kept, a new
// injection begins with none, and an argument wider than one unit is ignored.
void HoldCharacter(LPARAM argument) noexcept;
// Runs before the app sees an injected key-down for `window` on this thread.
// Every kept unit is posted to `window` with `keyLParam`, as WM_SYSCHAR when
// `system` (a WM_SYSKEYDOWN) and as WM_CHAR otherwise. Posting at this point
// means the app finds the character queued as soon as it starts on the
// key-down (INJ-9).
void KeyDown(HWND window, bool system, LPARAM keyLParam) noexcept;
// Runs on each message the thread takes: once the injection window has
// closed, the real keyboard state goes back. A single load when no thread
// is overridden.
void RestoreKeysIfDone() noexcept;
// Quick checks for the hooks: one load each
bool CharsWaiting() noexcept;
bool KeysReplaced() noexcept;

// For the helper's tests: whether this thread is inside an injected
// message, and inside the focus window
bool CursorFaked() noexcept;
bool FocusFaked() noexcept;

}// namespace OpenDesktopVR::AppHelper::InputFakes
