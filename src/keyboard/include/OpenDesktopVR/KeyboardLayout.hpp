// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Keyboard layouts for the virtual keyboard, read directly from the Windows
// keyboard layout DLLs (KBD*.DLL in System32) that are installed on this
// machine: no bundled layout data, no LoadKeyboardLayout(), no ToUnicodeEx().
//
// Physical keys are identified by their PC/AT 'set 1' scan code; extended
// keys have 0xE000 added (e.g. Right Ctrl is 0xE01D, Up is 0xE048), and the
// E1-prefixed Pause key is 0xE11D.
//
// This header only depends on Windows and the standard library.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::KeyboardLayout {

using ScanCode = uint16_t;

struct LayoutInfo {
  std::string mKLID;// "00000409"
  std::wstring mDisplayName;// "US", "Colemak", ...
  std::wstring mLayoutFile;// "KBDUS.DLL"

  bool operator==(const LayoutInfo&) const = default;
};

// Every layout in the registry whose DLL exists, sorted by name
std::vector<LayoutInfo> GetAvailableLayouts();
// The user's layouts, in GetKeyboardLayoutList() order, de-duplicated
std::vector<std::string> GetUserLayoutKLIDs();
// "" if unknown
std::string GetKLID(HKL);
std::optional<LayoutInfo> GetLayoutInfo(std::string_view klid);

struct ModifierState {
  bool mShift {false};
  bool mAltGr {false};// Ctrl+Alt on layouts with KLLF_ALTGR
  bool mCapsLock {false};
  bool operator==(const ModifierState&) const = default;
};

struct KeyOutput {
  enum class Kind : uint8_t { None, Text, DeadKey };
  Kind mKind {Kind::None};
  // Text: what to type (1+ UTF-16 units; ligatures can be several).
  // DeadKey: the accent character to show on the keycap
  std::wstring mText;

  bool operator==(const KeyOutput&) const = default;
};

// Immutable once loaded (the tables are copied out of the DLL, which is then
// unloaded), so it's safe to share between threads.
class Layout final {
 public:
  struct Impl;

  // nullptr (and no crash) on any failure
  static std::shared_ptr<const Layout> Load(std::string_view klid);

  ~Layout();
  Layout(const Layout&) = delete;
  Layout& operator=(const Layout&) = delete;

  const LayoutInfo& GetInfo() const noexcept;
  bool HasAltGr() const noexcept;
  // The extra ISO key (scan 0x56, between left Shift and Z) produces
  // something, and that something isn't just a copy of the ANSI backslash key
  // (the US layout maps 0x56 to another backslash, but US keyboards don't
  // have the key)
  bool IsISO() const noexcept;

  // 0 if unmapped; extended keys use the E0 (and E1) tables. Numpad keys give
  // their NumLock-off virtual key (e.g. VK_HOME for 0x47), like
  // MapVirtualKey(MAPVK_VSC_TO_VK)
  uint8_t GetVirtualKey(ScanCode) const noexcept;
  // Like GetVirtualKey(), but numpad keys give their NumLock-on virtual key
  // (VK_NUMPAD0-VK_NUMPAD9, VK_DECIMAL)
  uint8_t GetNumLockVirtualKey(ScanCode) const noexcept;
  // Prefers dedicated keys over the numpad: VK_HOME -> 0xE047, not 0x47.
  // VK_NUMPAD0-9 and VK_DECIMAL give the numpad keys; VK_SHIFT, VK_CONTROL
  // and VK_MENU give the left keys.
  std::optional<ScanCode> GetScanCode(uint8_t vk) const noexcept;
  bool IsNumpadKey(ScanCode) const noexcept;

  // Pure: no dead key state. Ctrl-only combinations aren't handled here, and
  // control characters (Enter, Tab, Backspace, Escape, ...) give Kind::None.
  // Numpad keys give their NumLock-on output.
  KeyOutput GetOutput(ScanCode, const ModifierState&) const;

  // Dead key composition; nullopt if the pair doesn't compose
  std::optional<wchar_t> Compose(wchar_t deadChar, wchar_t baseChar) const;
  // The pair composes into another dead key (DKF_DEAD)
  bool IsChainedDeadKey(wchar_t deadChar, wchar_t baseChar) const;
  // What Windows types for dead key + space: usually a spacing version of the
  // accent (e.g. '^', or '\'' on US-International). Falls back to the dead
  // character itself. Handy for keycap labels.
  wchar_t GetSpacingDeadChar(wchar_t deadChar) const;

 private:
  explicit Layout(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> p;
};

// Stateful typing: pending dead keys. Not thread-safe.
class Typist final {
 public:
  explicit Typist(std::shared_ptr<const Layout>);

  struct Result {
    // To send now (may be empty while a dead key is pending)
    std::wstring mText;
    // Accent waiting for the next key
    std::optional<wchar_t> mPendingDeadKey;

    bool operator==(const Result&) const = default;
  };

  // Windows behaviour: dead + composable base -> composed character (or a
  // new pending dead key, for chained dead keys); dead + anything else -> the
  // accent followed by that key's output; dead + space -> the accent alone.
  // The layout's dead key table always wins, so e.g. a '^' + '^' entry is
  // honoured.
  //
  // Backspace, Enter, Tab and Escape cancel the pending dead key; other keys
  // that don't type anything (e.g. a modifier combination with no output)
  // leave it pending.
  Result Press(ScanCode, const ModifierState&);
  // e.g. Backspace/Enter/arrows: the pending accent is dropped
  void CancelDeadKey();
  std::optional<wchar_t> GetPendingDeadKey() const;
  const std::shared_ptr<const Layout>& GetLayout() const;

 private:
  std::shared_ptr<const Layout> mLayout;
  std::optional<wchar_t> mPending;
};

}// namespace OpenDesktopVR::KeyboardLayout
