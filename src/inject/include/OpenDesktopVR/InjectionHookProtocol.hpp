// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// The part of the host <-> input-hook protocol that WindowKeyInjection needs.
//
// The host talks to the input hook running inside a captured app with a
// registered window message (RegisterWindowMessageW). The message name is the
// product's to choose; callers register it and pass the resulting message ID
// to WindowKeyInjection. wParam is a `Command`, lParam its argument.
//
// This header only covers the keyboard commands. The rest of the hook protocol
// (starting/ending an injection, etc.) is designed in the new project from the
// specification.
#pragma once

#include <Windows.h>

namespace OpenDesktopVR::InjectionHookProtocol {

enum class Command : WPARAM {
  // LPARAM: `KeyboardModifiers` bits. Sets which modifier keys the receiving
  // thread sees as held (GetKeyState() etc.) until the injection ends, when
  // the thread's own key state comes back; 0 restores it now.
  SetKeyboardModifiers = 0x10,
  // LPARAM: target top-level window. If the receiving thread's active window
  // isn't the target, makes it the active window on that thread only (never
  // the real foreground window). Some apps (e.g. Firefox) ignore text fields
  // in a window they don't consider active.
  ActivateWindow = 0x11,
  // LPARAM: a UTF-16 code unit. Typed (WM_CHAR) with the next key that goes
  // down in this injection: the hook posts it when that WM_KEYDOWN arrives, so
  // it's in the queue while the app handles the key, as TranslateMessage()
  // would have put it there. Apps (e.g. Chromium, Firefox) look for it then;
  // sent on its own afterwards, Firefox types Enter twice.
  QueueChar = 0x12,
};

// For `Command::SetKeyboardModifiers`
namespace KeyboardModifiers {
constexpr unsigned int Shift = 1 << 0;
constexpr unsigned int Control = 1 << 1;
constexpr unsigned int Alt = 1 << 2;
// Toggle states, like the lights on a real keyboard
constexpr unsigned int CapsLock = 1 << 3;
constexpr unsigned int NumLock = 1 << 4;
// The value is set, even if all the bits above are 0
constexpr unsigned int Set = 1u << 31;
}// namespace KeyboardModifiers

}// namespace OpenDesktopVR::InjectionHookProtocol
