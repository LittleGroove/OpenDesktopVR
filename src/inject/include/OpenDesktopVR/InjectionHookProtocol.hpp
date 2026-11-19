// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the clean side's own work, written from docs 06 and 07
// (C-2, 2026-11-19).
//
// The keyboard part of the host -> app helper channel (doc 06, "Keyboard
// injection" and the helper's command list; doc 07, "Modifier behavior").
//
// Transport: the helper's registered command message
// (AppHelperProtocol::CommandMessageName). wParam names a KeyCommand and
// lParam carries its argument. The injector's worker delivers these with
// SendMessageTimeout in one stream with the key messages themselves, so the
// helper's WH_CALLWNDPROC hook meets them on the app's UI thread in the order
// they were sent, ahead of the window procedure. The helper never replies.
//
// Every argument arrives from another process. The helper runs it through
// the Decode functions below and ignores whatever they reject.
#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace OpenDesktopVR::InjectionHookProtocol {

// Numbered in the order the injector issues them for one key press. The 0x30
// block keeps clear of every AppHelperProtocol::Command value; the helper's
// switch over sent commands holds both sets, so a clash with one of those
// would not compile.
enum class KeyCommand : uint8_t {
  // lParam: a top-level window that belongs to the receiving thread. When
  // that thread's active window is a different one, this window takes its
  // place, for that thread alone. The system-wide foreground window is left
  // where it is (R1). Firefox drops text input aimed at a window it doesn't
  // regard as active.
  ActivateOnThread = 0x30,
  // lParam: EncodeModifiers(...) or HandBackKeys. A modifier state replaces
  // what the thread's key-state functions report, and keeps doing so until
  // the injection and its grace period are over; then the thread's real
  // keyboard state returns. HandBackKeys returns it immediately instead.
  OverrideModifiers = 0x31,
  // lParam: EncodeCharacter(...). Kept back until the next key-down that the
  // injector delivers to this thread within the same injection, then posted
  // to that key's window with the key's lParam: WM_SYSCHAR if the key-down
  // was WM_SYSKEYDOWN (Alt held), WM_CHAR otherwise. Posting it at that
  // moment puts the character in the queue before the app starts on the
  // key-down, which is where a real keyboard would have it (INJ-9). An
  // injection that finishes with no key-down discards it.
  CharacterForNextKey = 0x32,
};

// How many CharacterForNextKey units one thread keeps at once; extra ones
// are discarded (doc 06)
inline constexpr std::size_t CharacterQueueLimit = 16;

// ---- Modifier state ---------------------------------------------------------
// Which of Shift, Ctrl and Alt are down, and whether Caps Lock and Num Lock
// are on. The virtual keyboard's modifiers stand for the left-hand keys.
// AltGr travels as Ctrl and Alt both down (doc 07).
struct InjectedModifiers {
  bool mShiftHeld {false};
  bool mCtrlHeld {false};
  bool mAltHeld {false};
  bool mCapsLockOn {false};
  bool mNumLockOn {false};
  bool operator==(const InjectedModifiers&) const = default;
};

// Wire layout of a modifier state: held keys in bits 8 to 10, lock toggles in
// bits 16 and 17, every other bit clear. Ctrl and Alt sit side by side, since
// AltGr is the pair. Because no marker bit is involved, 0 is an ordinary
// state: nothing held, both locks off.
namespace ModifierWire {
inline constexpr uint32_t CtrlHeld = 1u << 8;
inline constexpr uint32_t AltHeld = 1u << 9;
inline constexpr uint32_t ShiftHeld = 1u << 10;
inline constexpr uint32_t CapsLockOn = 1u << 16;
inline constexpr uint32_t NumLockOn = 1u << 17;
inline constexpr uint32_t StateMask = CtrlHeld | AltHeld | ShiftHeld | CapsLockOn | NumLockOn;
}// namespace ModifierWire

// The single OverrideModifiers argument that is not a state: stop
// overriding, and give the thread its real keyboard state back now. It has
// bits outside StateMask, so it can never be read as a state.
inline constexpr LPARAM HandBackKeys = static_cast<LPARAM>(0xFFFF'FFFFu);

inline constexpr LPARAM EncodeModifiers(const InjectedModifiers& state) noexcept {
  uint32_t wire = 0;
  wire |= state.mCtrlHeld ? ModifierWire::CtrlHeld : 0u;
  wire |= state.mAltHeld ? ModifierWire::AltHeld : 0u;
  wire |= state.mShiftHeld ? ModifierWire::ShiftHeld : 0u;
  wire |= state.mCapsLockOn ? ModifierWire::CapsLockOn : 0u;
  wire |= state.mNumLockOn ? ModifierWire::NumLockOn : 0u;
  return static_cast<LPARAM>(wire);
}

// The state an OverrideModifiers argument describes. nullopt for
// HandBackKeys (test for it first) and for any value with a bit outside
// StateMask, which this build never sends.
inline constexpr std::optional<InjectedModifiers> DecodeModifiers(LPARAM argument) noexcept {
  const auto wire = static_cast<ULONG_PTR>(argument);
  if ((wire & ~static_cast<ULONG_PTR>(ModifierWire::StateMask)) != 0) {
    return std::nullopt;
  }
  return InjectedModifiers {
    .mShiftHeld = (wire & ModifierWire::ShiftHeld) != 0,
    .mCtrlHeld = (wire & ModifierWire::CtrlHeld) != 0,
    .mAltHeld = (wire & ModifierWire::AltHeld) != 0,
    .mCapsLockOn = (wire & ModifierWire::CapsLockOn) != 0,
    .mNumLockOn = (wire & ModifierWire::NumLockOn) != 0,
  };
}

// ---- Characters -------------------------------------------------------------
// One UTF-16 code unit in the low 16 bits; a character outside the basic
// plane goes as two arguments, one per surrogate.
inline constexpr LPARAM EncodeCharacter(wchar_t unit) noexcept {
  return static_cast<LPARAM>(static_cast<uint16_t>(unit));
}

// nullopt for a value wider than one code unit
inline constexpr std::optional<wchar_t> DecodeCharacter(LPARAM argument) noexcept {
  const auto value = static_cast<ULONG_PTR>(argument);
  if (value > 0xFFFF) {
    return std::nullopt;
  }
  return static_cast<wchar_t>(value);
}

}// namespace OpenDesktopVR::InjectionHookProtocol
