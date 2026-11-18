// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// What the keyboard panel draws (doc 07 "Rendering"; ADR-036), shared
// between the threads that change it (the hand thread, the caret monitor,
// the UI thread) and the render thread that draws it. Every change that
// alters the picture wakes the renderer; a key press asks to be drawn at
// once, past the panel's 30-a-second cap. Hover changes only count when
// the hovered key or its 8-step level changes.
#pragma once

#include <OpenDesktopVR/KeyboardGeometry.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::VirtualKeyboard {

// How a key looks, apart from presses and hover
struct KeyFace {
  enum Flags : uint8_t {
    Dead = 1,   // a dead key
    Latched = 2,// a latched modifier
    Locked = 4, // a locked modifier
    LockOn = 8, // Caps Lock or Num Lock is on (its light)
    Pending = 16,// the dead key waiting for the next key
  };
  std::wstring mText;
  std::wstring mSmall;
  uint8_t mFlags {0};
  bool operator==(const KeyFace&) const = default;
};

// Changes with modifiers and layouts; immutable once shared
struct KeyboardFaces {
  std::shared_ptr<const Geometry> mGeometry;
  std::vector<KeyFace> mKeys;// as mGeometry->mKeys
  std::wstring mLayoutName;
  // An accent waiting for the next key, shown in the top strip
  std::optional<wchar_t> mPendingDeadKey;
};

struct KeyboardFrame {
  static constexpr std::size_t MaxHeld = 4;
  static constexpr std::size_t MaxFlashes = 4;
  static constexpr uint8_t HoverLevels = 8;
  static constexpr auto FlashTime = std::chrono::milliseconds(120);// doc 07

  std::shared_ptr<const KeyboardFaces> mFaces;
  bool mShown {false};
  // Where keys go: its panel's name; empty while there's none
  std::wstring mTarget;
  // The preview strip (K13): the panel showing the target, and the caret
  // in that panel's content pixels
  uint64_t mPreviewPanel {0};
  std::optional<RECT> mPreviewCaret;
  std::array<int, MaxHeld> mHeld {-1, -1, -1, -1};
  // Per hand: the hovered key (-1: none) and how near (1 to HoverLevels)
  std::array<int, 2> mHoverKey {-1, -1};
  std::array<uint8_t, 2> mHoverLevel {};
  std::array<int, MaxFlashes> mFlashKey {-1, -1, -1, -1};
  std::array<TimeUtil::TimePoint, MaxFlashes> mFlashUntil {};
  // K9: a hand is near the keyboard or typed lately
  bool mTyping {false};
  float mTypingOpacity {1.0f};
  float mIdleOpacity {1.0f};
  // A key typed with nowhere to go: "Tap a text field first" stands out
  TimeUtil::TimePoint mNoTargetUntil {};
};

class KeyboardModel {
 public:
  using Wake = std::function<void()>;

  // The keyboard's content source, while it exists (render thread)
  void SetWake(Wake wake);

  void SetFaces(std::shared_ptr<const KeyboardFaces> faces);
  void SetShown(bool shown);
  bool IsShown() const;
  void SetTarget(std::wstring name);
  void SetPreview(uint64_t panelID, std::optional<RECT> caret);
  void SetHeld(const std::array<int, KeyboardFrame::MaxHeld>& held);
  void SetHover(const std::array<int, 2>& keys, const std::array<uint8_t, 2>& levels);
  // A key was pressed: flash it for 120 ms, drawn at once
  void Flash(int key, TimeUtil::TimePoint now = TimeUtil::Now());
  void NoTarget(TimeUtil::TimePoint now = TimeUtil::Now());
  void SetTyping(bool typing);
  void SetOpacity(float typing, float idle);

  // Render thread
  KeyboardFrame Frame() const;
  // A key press since the last call: draw past the cap
  bool TakeUrgent() noexcept;

 private:
  // Called with the lock held; wakes after it's released
  template <class Change>
  void Update(Change&& change, bool urgent = false);

  mutable std::mutex mMutex;
  KeyboardFrame mFrame;
  Wake mWake;
  bool mUrgent {false};
};

}// namespace OpenDesktopVR::VirtualKeyboard
