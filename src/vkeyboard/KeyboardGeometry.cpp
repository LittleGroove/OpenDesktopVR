// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/KeyboardGeometry.hpp>

#include <cmath>

namespace OpenDesktopVR::VirtualKeyboard {

namespace {

constexpr float Margin = 0.25f;
constexpr float StripHeight = 1.0f;
constexpr float ClusterGap = 0.25f;
constexpr float MainWidth = 15.0f;
constexpr float NavWidth = 3.0f;
constexpr float NumpadWidth = 4.0f;
constexpr float TargetWidth = 5.0f;

class Builder {
 public:
  explicit Builder(std::vector<Key>& keys) : mKeys(keys) {
  }

  // One key at (x, y), in key units from the panel's top left
  Key& Add(float x, float y, float width, ScanCode scan, KeyRole role, std::wstring_view label = {}, bool repeats = false,
    float height = 1.0f) {
    Key key;
    key.mScanCode = scan;
    key.mRole = role;
    key.mRect = {x, y, width, height};
    key.mLabel = label;
    key.mRepeats = repeats;
    mKeys.push_back(key);
    return mKeys.back();
  }

  // A run of 1-unit character keys, scan codes in order
  void Characters(float x, float y, ScanCode first, int count) {
    for (int i = 0; i < count; ++i) {
      Add(x + static_cast<float>(i), y, 1.0f, static_cast<ScanCode>(first + i), KeyRole::Character, {}, true);
    }
  }

 private:
  std::vector<Key>& mKeys;
};

}// namespace

Geometry BuildGeometry(const GeometryOptions& options) {
  Geometry geometry;
  geometry.mOptions = options;
  Builder add {geometry.mKeys};
  geometry.mKeys.reserve(110);

  const float left = Margin;
  const float stripTop = Margin;
  geometry.mTopStrip = {left, stripTop, MainWidth + ClusterGap + NavWidth + ClusterGap + NumpadWidth, StripHeight};
  geometry.mTargetArea = {left, stripTop, TargetWidth, StripHeight};
  geometry.mPreviewArea
    = {left + TargetWidth + ClusterGap, stripTop, geometry.mTopStrip.mWidth - TargetWidth - ClusterGap, StripHeight};

  float top = stripTop + StripHeight + ClusterGap;
  if (options.mFunctionRow) {
    add.Add(left, top, 1, 0x01, KeyRole::Action, L"Esc");
    static constexpr std::wstring_view FunctionLabels[] {
      L"F1", L"F2", L"F3", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"};
    static constexpr ScanCode FunctionScans[] {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
    for (int i = 0; i < 12; ++i) {
      // Groups of four: F1 at 2, F5 at 6.5, F9 at 11 (F12 ends at 15)
      const float x = left + 2.0f + static_cast<float>(i) + (0.5f * static_cast<float>(i / 4));
      add.Add(x, top, 1, FunctionScans[i], KeyRole::Action, FunctionLabels[i]);
    }
    top += 1.0f + ClusterGap;
  }

  // The main block
  const float r0 = top;
  const float r1 = top + 1;
  const float r2 = top + 2;
  const float r3 = top + 3;
  const float r4 = top + 4;
  add.Add(left, r0, 1, 0x29, KeyRole::Character, {}, true);// ` ~
  add.Characters(left + 1, r0, 0x02, 12);// 1 to =
  add.Add(left + 13, r0, 2, 0x0E, KeyRole::Action, L"⌫ Backspace", true);

  add.Add(left, r1, 1.5f, 0x0F, KeyRole::Action, L"Tab");
  add.Characters(left + 1.5f, r1, 0x10, 12);// Q to ]
  if (options.mISO) {
    // The tall Enter: 1.5 wide on this row, 1.25 on the next
    auto& enter = add.Add(left + 13.5f, r1, 1.5f, 0x1C, KeyRole::Action, L"Enter");
    enter.mLower = KeyRect {left + 13.75f, r2, 1.25f, 1};
  } else {
    add.Add(left + 13.5f, r1, 1.5f, 0x2B, KeyRole::Character, {}, true);// \ |
  }

  add.Add(left, r2, 1.75f, 0x3A, KeyRole::CapsLock, L"Caps Lock");
  add.Characters(left + 1.75f, r2, 0x1E, 11);// A to '
  if (options.mISO) {
    add.Add(left + 12.75f, r2, 1, 0x2B, KeyRole::Character, {}, true);// # ~ beside Enter
  } else {
    add.Add(left + 12.75f, r2, 2.25f, 0x1C, KeyRole::Action, L"Enter");
  }

  float x = left;
  if (options.mISO) {
    add.Add(x, r3, 1.25f, 0x2A, KeyRole::Shift, L"Shift");
    add.Add(x + 1.25f, r3, 1, 0x56, KeyRole::Character, {}, true);// < >
    x += 2.25f;
  } else {
    add.Add(x, r3, 2.25f, 0x2A, KeyRole::Shift, L"Shift");
    x += 2.25f;
  }
  add.Characters(x, r3, 0x2C, 10);// Z to /
  add.Add(x + 10, r3, 2.75f, 0x36, KeyRole::Shift, L"Shift");

  add.Add(left, r4, 1.5f, 0x1D, KeyRole::Control, L"Ctrl");
  add.Add(left + 1.5f, r4, 1.25f, 0x38, KeyRole::Alt, L"Alt");
  add.Add(left + 2.75f, r4, 7, 0x39, KeyRole::Character, {}, true);// Space
  add.Add(left + 9.75f, r4, 1.25f, 0xE038, KeyRole::AltGr, L"AltGr");
  add.Add(left + 11, r4, 1.25f, 0, KeyRole::Layout, L"Layout");
  add.Add(left + 12.25f, r4, 1.25f, 0, KeyRole::Hide, L"Hide");
  add.Add(left + 13.5f, r4, 1.5f, 0xE01D, KeyRole::Control, L"Ctrl");

  // Navigation and arrows
  const float nav = left + MainWidth + ClusterGap;
  add.Add(nav, r0, 1, 0xE052, KeyRole::Action, L"Ins");
  add.Add(nav + 1, r0, 1, 0xE047, KeyRole::Action, L"Home", true);
  add.Add(nav + 2, r0, 1, 0xE049, KeyRole::Action, L"PgUp", true);
  add.Add(nav, r1, 1, 0xE053, KeyRole::Action, L"Del", true);
  add.Add(nav + 1, r1, 1, 0xE04F, KeyRole::Action, L"End", true);
  add.Add(nav + 2, r1, 1, 0xE051, KeyRole::Action, L"PgDn", true);
  add.Add(nav + 1, r3, 1, 0xE048, KeyRole::Action, L"↑", true);
  add.Add(nav, r4, 1, 0xE04B, KeyRole::Action, L"←", true);
  add.Add(nav + 1, r4, 1, 0xE050, KeyRole::Action, L"↓", true);
  add.Add(nav + 2, r4, 1, 0xE04D, KeyRole::Action, L"→", true);

  // The numpad
  const float pad = nav + NavWidth + ClusterGap;
  add.Add(pad, r0, 1, 0x45, KeyRole::NumLock, L"Num");
  add.Add(pad + 1, r0, 1, 0xE035, KeyRole::Character, {}, true);// /
  add.Add(pad + 2, r0, 1, 0x37, KeyRole::Character, {}, true);  // *
  add.Add(pad + 3, r0, 1, 0x4A, KeyRole::Character, {}, true);  // -
  struct PadKey {
    float mX;
    float mY;
    float mWidth;
    ScanCode mScan;
    std::wstring_view mSecond;
  };
  const PadKey padKeys[] {
    {0, r1, 1, 0x47, L"Home"},
    {1, r1, 1, 0x48, L"↑"},
    {2, r1, 1, 0x49, L"PgUp"},
    {0, r2, 1, 0x4B, L"←"},
    {1, r2, 1, 0x4C, L""},
    {2, r2, 1, 0x4D, L"→"},
    {0, r3, 1, 0x4F, L"End"},
    {1, r3, 1, 0x50, L"↓"},
    {2, r3, 1, 0x51, L"PgDn"},
    {0, r4, 2, 0x52, L"Ins"},
    {2, r4, 1, 0x53, L"Del"},
  };
  for (const auto& k: padKeys) {
    auto& key = add.Add(pad + k.mX, k.mY, k.mWidth, k.mScan, KeyRole::Numpad, {}, true);
    key.mSecondLabel = k.mSecond;
  }
  add.Add(pad + 3, r1, 1, 0x4E, KeyRole::Character, {}, true, 2);// + (tall)
  add.Add(pad + 3, r3, 1, 0xE01C, KeyRole::Action, L"Enter", false, 2);// tall

  geometry.mWidth = Margin + geometry.mTopStrip.mWidth + Margin;
  geometry.mHeight = r4 + 1 + Margin;
  return geometry;
}

int HitTest(const Geometry& geometry, float u, float v) noexcept {
  if (!std::isfinite(u) || !std::isfinite(v)) {
    return -1;
  }
  const float x = u * geometry.mWidth;
  const float y = v * geometry.mHeight;
  for (std::size_t i = 0; i < geometry.mKeys.size(); ++i) {
    const auto& key = geometry.mKeys[i];
    if (key.mRect.Contains(x, y) || (key.mLower && key.mLower->Contains(x, y))) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FindKey(const Geometry& geometry, ScanCode scanCode) noexcept {
  for (std::size_t i = 0; i < geometry.mKeys.size(); ++i) {
    if (geometry.mKeys[i].mScanCode == scanCode && scanCode != 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FindRole(const Geometry& geometry, KeyRole role) noexcept {
  for (std::size_t i = 0; i < geometry.mKeys.size(); ++i) {
    if (geometry.mKeys[i].mRole == role) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::pair<float, float> KeyCentre(const Geometry& geometry, int key) noexcept {
  if (key < 0 || static_cast<std::size_t>(key) >= geometry.mKeys.size() || geometry.mWidth <= 0 || geometry.mHeight <= 0) {
    return {0.5f, 0.5f};
  }
  const auto& rect = geometry.mKeys[static_cast<std::size_t>(key)].mRect;
  return {(rect.mX + (rect.mWidth / 2)) / geometry.mWidth, (rect.mY + (rect.mHeight / 2)) / geometry.mHeight};
}

float DistanceFromCentre(const Geometry& geometry, int key, float u, float v) noexcept {
  const auto [cu, cv] = KeyCentre(geometry, key);
  const float dx = (u - cu) * geometry.mWidth;
  const float dy = (v - cv) * geometry.mHeight;
  return std::sqrt((dx * dx) + (dy * dy));
}

}// namespace OpenDesktopVR::VirtualKeyboard
