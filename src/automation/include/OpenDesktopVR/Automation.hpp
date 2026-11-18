// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Automation (UI-9, doc 09 "Automation"; ADR-042): commands for Stream
// Deck, VoiceAttack and other tools that can run a program.
//
//   OpenDesktopVR.exe toggle Discord
//   OpenDesktopVR.exe nudge "#2" left 2 --repeat 3
//
// The program sees a first word that isn't an option, sends the words to
// the running host and exits; it never becomes the host. This module is
// the pure part: the words parsed into a command,
// panels found by name, list position or ID, and the words packed for the
// trip to the host and unpacked there (no windows or messages here).
// Everything from outside is
// untrusted (TH-9): bounded, parsed without exceptions, and refused with a
// reason rather than guessed at.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace OpenDesktopVR::Automation {

// Limits on what's accepted (TH-9)
inline constexpr std::size_t MaxWords = 16;
inline constexpr std::size_t MaxWordLength = 256;
inline constexpr int MaxRepeat = 20;

enum class Verb : uint8_t {
  Show,
  Hide,
  Toggle,
  Opacity,
  Brightness,
  Tint,
  Nudge,
  Window,
  Recenter,
  Profile,
  Keyboard,
  Menu,
  Help,
};

// What a command acts on
struct Target {
  enum class Kind : uint8_t { None, All, Keyboard, Panel };
  Kind mKind {Kind::None};
  // Panel: a name (without case), "#2" (the second in the panel list), or a
  // panel's 16-digit ID
  std::wstring mSelector;
  bool operator==(const Target&) const = default;
};

// On, off or the other way (tint; the keyboard: show/hide; the menu:
// open/close)
enum class Switch : uint8_t { On, Off, Toggle };

enum class Direction : uint8_t {
  Left,
  Right,
  Up,
  Down,
  Closer,
  Farther,// centimetres
  Bigger,
  Smaller,// percent of its width
  TurnLeft,
  TurnRight,
  TiltBack,
  TiltForward,// degrees
};

struct Command {
  Verb mVerb {Verb::Help};
  Target mTarget;
  Switch mSwitch {Switch::Toggle};
  // Opacity and brightness: a percentage to set (10 to 100), or to add
  // (negative to take away)
  float mPercent {0};
  bool mRelative {false};
  // Nudge: which way and how far (centimetres, percent or degrees)
  Direction mDirection {Direction::Left};
  float mAmount {0};
  // Window: text in its title; Profile: its name ("" with a step)
  std::wstring mText;
  int mProfileStep {0};// Profile: +1 next, -1 previous
  int mRepeat {1};
  bool operator==(const Command&) const = default;
};

struct Parsed {
  std::optional<Command> mCommand;
  std::string mError;// why not, if not: for the host's log
};

// The words after the program's name. Never throws.
Parsed Parse(std::span<const std::wstring> words) noexcept;

// True if these words are a command rather than the host's own options:
// the first one isn't an option ("--...")
bool IsCommand(std::span<const std::wstring> words) noexcept;

// A panel as commands see it
struct PanelName {
  uint64_t mID {0};
  std::wstring mName;
};
// The panels a selector names: every panel with that name (without case),
// else "#N" (1-based, in the list's order), else a 16-digit ID. Empty if
// none.
std::vector<uint64_t> Resolve(const std::wstring& selector, std::span<const PanelName> panels);

// For the trip to the host (WM_COPYDATA): the words, each ending in a NUL,
// as UTF-16, under a tag that says what they are
inline constexpr uintptr_t WireTag = 0x4F445631;// "ODV1"
// One word and one character past the limits fit, so the host can say
// which limit was passed
inline constexpr std::size_t MaxWireBytes = (MaxWords + 1) * (MaxWordLength + 2) * sizeof(wchar_t);
std::vector<wchar_t> Pack(std::span<const std::wstring> words);
// nullopt if it isn't well formed: the wrong tag, too big, an odd size, or
// no final NUL. Parse checks the words' number and length.
std::optional<std::vector<std::wstring>> Unpack(uintptr_t tag, const void* data, std::size_t bytes) noexcept;

// What the program answers (its exit code) and the host replies
enum class Reply : int {
  Done = 0,
  Refused = 1,   // not a command, or no such panel or profile: see the host's log
  NotRunning = 2,// no host to send it to
  NoAnswer = 3,  // the host didn't answer within 3 seconds, or was too busy
};

// The commands, for "help" and the settings window
std::wstring Usage();

// For logs: "nudge #2 left 2 cm"
std::string Describe(const Command& command);

}// namespace OpenDesktopVR::Automation
