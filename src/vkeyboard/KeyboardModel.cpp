// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/KeyboardModel.hpp>

#include <algorithm>
#include <cmath>

namespace OpenDesktopVR::VirtualKeyboard {

namespace {

bool SameRect(const std::optional<RECT>& a, const std::optional<RECT>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return !a || (a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom);
}

}// namespace

template <class Change>
void KeyboardModel::Update(Change&& change, bool urgent) {
  Wake wake;
  {
    std::scoped_lock lock(mMutex);
    if (!change(mFrame)) {
      return;
    }
    mUrgent = mUrgent || urgent;
    wake = mWake;
  }
  if (wake) {
    wake();
  }
}

void KeyboardModel::SetWake(Wake wake) {
  std::scoped_lock lock(mMutex);
  mWake = std::move(wake);
}

void KeyboardModel::SetFaces(std::shared_ptr<const KeyboardFaces> faces) {
  Update([&](KeyboardFrame& frame) {
    frame.mFaces = std::move(faces);
    return true;
  });
}

void KeyboardModel::SetShown(bool shown) {
  Update([&](KeyboardFrame& frame) {
    if (frame.mShown == shown) {
      return false;
    }
    frame.mShown = shown;
    if (!shown) {
      frame.mHoverKey = {-1, -1};
      frame.mHeld = {-1, -1, -1, -1};
    }
    return true;
  }, true);
}

bool KeyboardModel::IsShown() const {
  std::scoped_lock lock(mMutex);
  return mFrame.mShown;
}

void KeyboardModel::SetTarget(std::wstring name) {
  Update([&](KeyboardFrame& frame) {
    if (frame.mTarget == name) {
      return false;
    }
    frame.mTarget = std::move(name);
    return true;
  });
}

void KeyboardModel::SetPreview(uint64_t panelID, std::optional<RECT> caret) {
  Update([&](KeyboardFrame& frame) {
    if (frame.mPreviewPanel == panelID && SameRect(frame.mPreviewCaret, caret)) {
      return false;
    }
    frame.mPreviewPanel = panelID;
    frame.mPreviewCaret = caret;
    return true;
  });
}

void KeyboardModel::SetHeld(const std::array<int, KeyboardFrame::MaxHeld>& held) {
  Update([&](KeyboardFrame& frame) {
    if (frame.mHeld == held) {
      return false;
    }
    frame.mHeld = held;
    return true;
  }, true);
}

void KeyboardModel::SetHover(const std::array<int, 2>& keys, const std::array<uint8_t, 2>& levels) {
  Update([&](KeyboardFrame& frame) {
    if (frame.mHoverKey == keys && frame.mHoverLevel == levels) {
      return false;
    }
    frame.mHoverKey = keys;
    frame.mHoverLevel = levels;
    return true;
  });
}

void KeyboardModel::Flash(int key, TimeUtil::TimePoint now) {
  Update([&](KeyboardFrame& frame) {
    // The slot of this key, or the one ending soonest
    std::size_t slot = 0;
    for (std::size_t i = 0; i < KeyboardFrame::MaxFlashes; ++i) {
      if (frame.mFlashKey[i] == key) {
        slot = i;
        break;
      }
      if (frame.mFlashUntil[i] < frame.mFlashUntil[slot]) {
        slot = i;
      }
    }
    frame.mFlashKey[slot] = key;
    frame.mFlashUntil[slot] = now + KeyboardFrame::FlashTime;
    return true;
  }, true);
}

void KeyboardModel::NoTarget(TimeUtil::TimePoint now) {
  Update([&](KeyboardFrame& frame) {
    frame.mNoTargetUntil = now + std::chrono::milliseconds(800);
    return true;
  }, true);
}

void KeyboardModel::SetTyping(bool typing) {
  Update([&](KeyboardFrame& frame) {
    if (frame.mTyping == typing) {
      return false;
    }
    frame.mTyping = typing;
    // Only a different opacity changes the picture
    return frame.mTypingOpacity != frame.mIdleOpacity;
  });
}

void KeyboardModel::SetOpacity(float typing, float idle) {
  const auto clean = [](float value) { return std::isfinite(value) ? std::clamp(value, 0.1f, 1.0f) : 1.0f; };
  Update([&](KeyboardFrame& frame) {
    const float t = clean(typing);
    const float i = clean(idle);
    if (frame.mTypingOpacity == t && frame.mIdleOpacity == i) {
      return false;
    }
    frame.mTypingOpacity = t;
    frame.mIdleOpacity = i;
    return true;
  });
}

KeyboardFrame KeyboardModel::Frame() const {
  std::scoped_lock lock(mMutex);
  return mFrame;
}

bool KeyboardModel::TakeUrgent() noexcept {
  std::scoped_lock lock(mMutex);
  return std::exchange(mUrgent, false);
}

}// namespace OpenDesktopVR::VirtualKeyboard
