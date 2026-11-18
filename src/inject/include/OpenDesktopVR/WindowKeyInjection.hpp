// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Typing into a window that isn't the foreground window (e.g. a captured app
// while a game has the real keyboard focus), by sending it the window
// messages a real key press produces. Only depends on Windows, so it can be
// tested on its own.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace OpenDesktopVR {

// One key press: the key goes down, types its text (if any), and comes up
struct VirtualKeyStroke {
  // PC/AT scan code set 1; extended keys (e.g. arrows) are 0xE0xx
  uint16_t mScanCode {};
  // 0 if the key only types text, e.g. a character that isn't on the app's
  // keyboard layout
  uint8_t mVirtualKey {};
  // What the key types (WM_CHAR), e.g. "a", "\r" for Enter, "\b" for
  // Backspace; empty for keys that don't type, e.g. arrows or Ctrl+C
  std::wstring mText;

  bool mShift {false};
  bool mControl {false};
  bool mAlt {false};
  bool mCapsLock {false};
  bool mNumLock {false};
  // Sent again while the key is held (auto-repeat)
  bool mRepeat {false};

  bool operator==(const VirtualKeyStroke&) const = default;
};

namespace WindowKeyInjection {

struct Message {
  UINT mMessage {};
  WPARAM mWParam {};
  LPARAM mLParam {};
  bool operator==(const Message&) const = default;
};

// Keys that would close the app, switch away from the game, or leave the app
// waiting in a menu that isn't visible in VR (e.g. Alt+F4, Alt+Tab, a lone
// Alt or F10)
bool IsBlocked(const VirtualKeyStroke&);

// The messages for a stroke, in order, to send (not post) to the window. The
// text goes as `InjectionHookProtocol::Command::QueueChar` messages (using
// `hookMessage`, the registered window message the input hook listens for)
// before the key: the hook queues the characters as the key goes down, like
// TranslateMessage(). Alt combinations become WM_SYSKEYDOWN/WM_SYSKEYUP
// without characters, so the app's default handling doesn't open a menu.
// Empty if blocked.
std::vector<Message> GetMessages(const VirtualKeyStroke&, UINT hookMessage);

// For `InjectionHookProtocol::Command::SetKeyboardModifiers`
LPARAM GetModifierBits(const VirtualKeyStroke&);

// Where key messages for a captured window should go: the focused window on
// that window's thread, if it belongs to the captured window; otherwise the
// captured window itself
HWND GetKeyTarget(HWND capturedTopLevel);

}// namespace WindowKeyInjection
}// namespace OpenDesktopVR
