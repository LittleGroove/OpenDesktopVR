// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The virtual keyboard's keys (doc 07 "Layout and geometry", ADR-036): a
// full-size keyboard in key units (1 = a letter key's pitch), ANSI or ISO,
// with or without the function row, then the navigation keys, the arrows
// and the numpad. A strip across the top holds where keys go and the
// preview. Pure data and arithmetic: no layout tables, no drawing.
//
// Left out, so nothing reaches Windows or opens a menu that can't be seen
// in VR: the Windows keys, Menu, Print Screen, Scroll Lock and Pause (K7).
// The bottom row has a layout key and a hide key instead.
#pragma once

#include <OpenDesktopVR/KeyboardLayout.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenDesktopVR::VirtualKeyboard {

using KeyboardLayout::ScanCode;

enum class KeyRole : uint8_t {
  Character,// what it types comes from the layout (letters, digits, Space)
  Action,   // Enter, Backspace, Tab, Esc, the arrows, navigation, F1-F12
  Numpad,   // the numpad's digits and decimal: Num Lock decides what they do
  Shift,
  Control,
  Alt,
  AltGr,// the right Alt: AltGr on layouts that have it, Alt on others
  CapsLock,
  NumLock,
  Layout,// the next layout (K8)
  Hide,  // hides the keyboard
};

// A rectangle in key units, from the panel's top left
struct KeyRect {
  float mX {0};
  float mY {0};
  float mWidth {0};
  float mHeight {0};
  bool Contains(float x, float y) const noexcept {
    return x >= mX && x < mX + mWidth && y >= mY && y < mY + mHeight;
  }
  bool operator==(const KeyRect&) const = default;
};

struct Key {
  ScanCode mScanCode {0};// 0 for the layout and hide keys
  KeyRole mRole {KeyRole::Character};
  // A key's whole pitch: neighbours touch, and the gap is only drawn
  KeyRect mRect;
  // ISO Enter's lower, narrower part (it's L-shaped)
  std::optional<KeyRect> mLower;
  // A fixed legend for keys that don't type from the layout; empty: from
  // the layout
  std::wstring_view mLabel;
  // The numpad's Num Lock off meanings ("Home", "End")
  std::wstring_view mSecondLabel;
  // Held keys repeat (K12): characters, Space, Backspace, Delete, the
  // arrows and navigation, the numpad. Not Enter, Esc, Tab, the function
  // keys, modifiers or locks (ADR-036).
  bool mRepeats {false};
  bool operator==(const Key&) const = default;
};

struct GeometryOptions {
  bool mISO {false};// the tall Enter and the extra key beside left Shift
  bool mFunctionRow {true};
  bool operator==(const GeometryOptions&) const = default;
};

struct Geometry {
  GeometryOptions mOptions;
  std::vector<Key> mKeys;
  // The whole panel, in key units (margins included)
  float mWidth {0};
  float mHeight {0};
  // Across the top: where keys go, and the preview (K13)
  KeyRect mTopStrip;
  KeyRect mTargetArea;// the target's name, at the strip's left
  KeyRect mPreviewArea;// the rest of the strip
};

// Key units to the panel texture's pixels (doc 07: 64 was too small)
inline constexpr float PixelsPerUnit = 80;
// The gap drawn between keys, in key units
inline constexpr float KeyGap = 0.1f;

Geometry BuildGeometry(const GeometryOptions& options);

// The key under (u, v), 0 to 1 from the panel's top left, or -1 if none
// (between the clusters, the margins, the top strip)
int HitTest(const Geometry& geometry, float u, float v) noexcept;

// The first key with this scan code, or this role (-1 if none)
int FindKey(const Geometry& geometry, ScanCode scanCode) noexcept;
int FindRole(const Geometry& geometry, KeyRole role) noexcept;

// The middle of a key as (u, v) (its upper part for ISO Enter)
std::pair<float, float> KeyCentre(const Geometry& geometry, int key) noexcept;

// From (u, v) to the middle of `key`, in key units (accuracy statistics)
float DistanceFromCentre(const Geometry& geometry, int key, float u, float v) noexcept;

}// namespace OpenDesktopVR::VirtualKeyboard
