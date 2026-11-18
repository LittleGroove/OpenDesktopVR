// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
#include <OpenDesktopVR/InjectionHookProtocol.hpp>
#include <OpenDesktopVR/WindowKeyInjection.hpp>

namespace OpenDesktopVR::WindowKeyInjection {

namespace {

bool IsAltKey(uint8_t vk) {
  return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}

// lParam for WM_KEYDOWN and friends: repeat count, scan code, extended key,
// context code (Alt held), previous key state, transition state
LPARAM KeyLParam(
  const VirtualKeyStroke& stroke,
  const bool isSystem,
  const bool up) {
  LPARAM ret = 1;
  ret |= static_cast<LPARAM>(stroke.mScanCode & 0xff) << 16;
  if ((stroke.mScanCode & 0xff00) == 0xe000) {
    ret |= LPARAM {1} << 24;
  }
  if (isSystem) {
    ret |= LPARAM {1} << 29;
  }
  if (up || stroke.mRepeat) {
    ret |= LPARAM {1} << 30;
  }
  if (up) {
    ret |= LPARAM {1} << 31;
  }
  return ret;
}

}// namespace

bool IsBlocked(const VirtualKeyStroke& stroke) {
  const auto vk = stroke.mVirtualKey;
  if (vk == 0) {
    return false;
  }
  // The Windows key is left out of the virtual keyboard entirely; Alt alone
  // (or Alt + another modifier) opens the app's menu
  if (vk == VK_LWIN || vk == VK_RWIN || IsAltKey(vk)) {
    return true;
  }
  if (stroke.mAlt && !stroke.mControl) {
    switch (vk) {
      case VK_F4:// closes the app
      case VK_TAB:// switches windows
      case VK_ESCAPE:// switches windows
      case VK_SPACE:// opens the window menu
        return true;
    }
  }
  // Handled by Windows, not apps; also never useful in a captured window
  if (stroke.mControl && stroke.mAlt && vk == VK_DELETE) {
    return true;
  }
  if (stroke.mControl && vk == VK_ESCAPE) {
    return true;
  }
  return false;
}

std::vector<Message> GetMessages(
  const VirtualKeyStroke& stroke,
  const UINT hookMessage) {
  if (IsBlocked(stroke)) {
    return {};
  }

  std::vector<Message> ret;
  if (stroke.mVirtualKey == 0) {
    // Text only, e.g. a character that isn't on the app's keyboard layout
    for (const auto ch: stroke.mText) {
      ret.push_back({WM_CHAR, ch, 1});
    }
    return ret;
  }

  // Alt (without Ctrl, which with Alt is AltGr) makes these 'system' keys.
  // F10 is always a system key on a real keyboard; it's sent as a normal key,
  // as the app's default handling would open the window's menu.
  const bool isSystem = stroke.mAlt && !stroke.mControl
    && stroke.mVirtualKey != VK_F10;
  const auto vk = static_cast<WPARAM>(stroke.mVirtualKey);
  // The characters are queued by the input hook as the key goes down, as
  // TranslateMessage() would; apps look for them while handling the key. No
  // WM_SYSCHAR: the app's default handling of WM_SYSCHAR opens menus, and apps
  // act on Alt shortcuts when the key goes down.
  if (!isSystem) {
    for (const auto ch: stroke.mText) {
      ret.push_back({
        hookMessage,
        static_cast<WPARAM>(InjectionHookProtocol::Command::QueueChar),
        static_cast<LPARAM>(ch),
      });
    }
  }
  ret.push_back({
    static_cast<UINT>(isSystem ? WM_SYSKEYDOWN : WM_KEYDOWN),
    vk,
    KeyLParam(stroke, isSystem, /* up = */ false),
  });
  ret.push_back({
    static_cast<UINT>(isSystem ? WM_SYSKEYUP : WM_KEYUP),
    vk,
    KeyLParam(stroke, isSystem, /* up = */ true),
  });
  return ret;
}

LPARAM GetModifierBits(const VirtualKeyStroke& stroke) {
  namespace Mods = InjectionHookProtocol::KeyboardModifiers;
  LPARAM ret = Mods::Set;
  if (stroke.mShift) {
    ret |= Mods::Shift;
  }
  if (stroke.mControl) {
    ret |= Mods::Control;
  }
  if (stroke.mAlt) {
    ret |= Mods::Alt;
  }
  if (stroke.mCapsLock) {
    ret |= Mods::CapsLock;
  }
  if (stroke.mNumLock) {
    ret |= Mods::NumLock;
  }
  return ret;
}

HWND GetKeyTarget(HWND capturedTopLevel) {
  if (!capturedTopLevel) {
    return {};
  }
  const auto thread = GetWindowThreadProcessId(capturedTopLevel, nullptr);
  GUITHREADINFO info {.cbSize = sizeof(GUITHREADINFO)};
  if (thread && GetGUIThreadInfo(thread, &info) && info.hwndFocus) {
    if (GetAncestor(info.hwndFocus, GA_ROOT) == capturedTopLevel) {
      return info.hwndFocus;
    }
  }
  return capturedTopLevel;
}

}// namespace OpenDesktopVR::WindowKeyInjection
