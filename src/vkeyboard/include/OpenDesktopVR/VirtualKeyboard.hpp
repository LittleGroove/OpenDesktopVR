// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// What the virtual keyboard's keys do (doc 07 "Modifier behavior", K7, K8,
// K12; ADR-036): key presses in, key strokes out. Pure logic, with time
// passed in; not thread-safe (the host's hand thread owns it).
//
// - Shift, Ctrl, Alt and AltGr: one tap latches for the next key; a second
//   tap within 400 ms locks; tapping a locked one, or a latched one later,
//   clears it. Both Shifts are one Shift, both Ctrls one Ctrl.
// - Caps Lock and Num Lock toggle; their state goes with every stroke.
// - AltGr (on layouts that have it) is Ctrl+Alt with the layout's AltGr
//   characters, as is Ctrl+Alt itself there.
// - Ctrl+letter carries its control character (Ctrl+C: 0x03), as
//   TranslateMessage would; Alt combinations carry none.
// - Dead keys compose through the imported `Typist`: a dead key alone
//   sends nothing; the next key sends the result.
// - Held keys repeat after Windows' delay, at its rate, never faster than
//   every 50 ms (VK-3); the newest held key repeats.
#pragma once

#include <OpenDesktopVR/KeyboardGeometry.hpp>
#include <OpenDesktopVR/KeyboardLayout.hpp>
#include <OpenDesktopVR/WindowKeyInjection.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::VirtualKeyboard {

enum class Modifier : uint8_t { Shift, Control, Alt, AltGr };
inline constexpr std::size_t ModifierCount = 4;

enum class ModifierState : uint8_t { Off, Latched, Locked };

struct RepeatTiming {
  double mDelaySeconds {0.5};
  double mIntervalSeconds {1.0 / 30.0};

  static constexpr double MinimumInterval = 0.05;// VK-3
  static constexpr double LockTime = 0.4;// a second modifier tap within this locks

  // From Windows' settings: the delay setting (0 to 3: 250 ms each, plus
  // 250) and the speed setting (0 to 31: about 2.5 to 30 a second), with
  // VK-3's floor
  static RepeatTiming FromWindows(int delaySetting, int speedSetting) noexcept;
  // Reads them (SPI_GETKEYBOARDDELAY, SPI_GETKEYBOARDSPEED)
  static RepeatTiming FromSystem() noexcept;
  bool operator==(const RepeatTiming&) const = default;
};

struct PressResult {
  // To send to the target window (none for modifiers, locks, a dead key on
  // its own, the layout and hide keys)
  std::optional<VirtualKeyStroke> mStroke;
  bool mEnter {false};// Enter or the numpad's Enter
  bool mHide {false};// the hide key
  bool mLayoutChanged {false};// the geometry may have changed too (ISO)
  bool mChanged {false};// something drawn changed (legends, states)
};

// A key's legend as things are now
struct Legend {
  std::wstring mText;
  bool mDead {false};// a dead key: it waits for the next key
  // Small, in a corner: the AltGr character (character keys), or the
  // numpad key's Num Lock off meaning
  std::wstring mSmall;
};

class Keyboard {
 public:
  using LayoutPtr = std::shared_ptr<const KeyboardLayout::Layout>;

  // `layouts` may be empty (the keys then type nothing); `active` is the
  // one in use
  explicit Keyboard(std::vector<LayoutPtr> layouts = {}, std::size_t active = 0, bool functionRow = true);

  // Keeps the active layout if it's still in the list
  void SetLayouts(std::vector<LayoutPtr> layouts, std::size_t active);
  void SetFunctionRow(bool functionRow);
  void SetRepeatTiming(const RepeatTiming& timing) noexcept {
    mRepeatTiming = timing;
  }

  const std::shared_ptr<const Geometry>& GetGeometry() const noexcept {
    return mGeometry;
  }
  const KeyboardLayout::Layout* ActiveLayout() const noexcept;
  std::size_t LayoutCount() const noexcept {
    return mLayouts.size();
  }

  PressResult Press(int key, double now);
  void Release(int key);
  // A repeat of the newest held key, if one is due (at most one per call:
  // falling behind never sends a burst)
  std::optional<VirtualKeyStroke> Repeat(double now);
  // Lets go of everything held (the keyboard hid, tracking stopped)
  void ReleaseAll() noexcept;
  // Clears latched and locked modifiers and any pending dead key (shown
  // again later, the keyboard starts plain); keeps Caps and Num Lock
  void ClearModifiers() noexcept;

  ModifierState GetModifier(Modifier modifier) const noexcept {
    return mModifiers[static_cast<std::size_t>(modifier)];
  }
  bool CapsLock() const noexcept {
    return mCapsLock;
  }
  bool NumLock() const noexcept {
    return mNumLock;
  }
  std::optional<wchar_t> PendingDeadKey() const;
  bool IsHeld(int key) const noexcept;
  bool IsRepeating() const noexcept {
    return mRepeatKey >= 0;
  }
  // The right Alt is AltGr on this layout
  bool HasAltGr() const noexcept;

  Legend GetLegend(int key) const;

 private:
  void Rebuild();
  void TapModifier(Modifier modifier, double now);
  bool IsOn(Modifier modifier) const noexcept {
    return GetModifier(modifier) != ModifierState::Off;
  }
  // What the layout sees: Shift, AltGr, Caps Lock
  KeyboardLayout::ModifierState LayoutState() const noexcept;

  std::vector<LayoutPtr> mLayouts;
  std::size_t mActive {0};
  std::optional<KeyboardLayout::Typist> mTypist;
  bool mFunctionRow {true};
  std::shared_ptr<const Geometry> mGeometry;
  RepeatTiming mRepeatTiming;

  std::array<ModifierState, ModifierCount> mModifiers {};
  std::array<double, ModifierCount> mLatchedAt {};
  bool mCapsLock {false};
  bool mNumLock {true};

  std::vector<int> mHeld;// at most a few (one per finger)
  int mRepeatKey {-1};
  VirtualKeyStroke mRepeatStroke;
  double mNextRepeat {0};
};

}// namespace OpenDesktopVR::VirtualKeyboard
