// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/VirtualKeyboard.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace OpenDesktopVR::VirtualKeyboard {

using KeyboardLayout::KeyOutput;

RepeatTiming RepeatTiming::FromWindows(int delaySetting, int speedSetting) noexcept {
  RepeatTiming timing;
  timing.mDelaySeconds = 0.25 * static_cast<double>(std::clamp(delaySetting, 0, 3) + 1);
  // Windows' 0 to 31 is about 2.5 to 30 repeats a second
  const double perSecond = 2.5 + ((30.0 - 2.5) * static_cast<double>(std::clamp(speedSetting, 0, 31)) / 31.0);
  timing.mIntervalSeconds = std::max(MinimumInterval, 1.0 / perSecond);
  return timing;
}

RepeatTiming RepeatTiming::FromSystem() noexcept {
  int delay = 1;
  DWORD speed = 31;
  SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
  SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
  return FromWindows(delay, static_cast<int>(speed));
}

Keyboard::Keyboard(std::vector<LayoutPtr> layouts, std::size_t active, bool functionRow) : mFunctionRow(functionRow) {
  SetLayouts(std::move(layouts), active);
}

void Keyboard::SetLayouts(std::vector<LayoutPtr> layouts, std::size_t active) {
  std::erase(layouts, nullptr);
  const auto* current = ActiveLayout();
  mLayouts = std::move(layouts);
  mActive = std::min(active, mLayouts.empty() ? std::size_t {0} : mLayouts.size() - 1);
  if (current) {
    for (std::size_t i = 0; i < mLayouts.size(); ++i) {
      if (mLayouts[i]->GetInfo().mKLID == current->GetInfo().mKLID) {
        mActive = i;
        break;
      }
    }
  }
  Rebuild();
}

void Keyboard::SetFunctionRow(bool functionRow) {
  if (functionRow != mFunctionRow) {
    mFunctionRow = functionRow;
    Rebuild();
  }
}

const KeyboardLayout::Layout* Keyboard::ActiveLayout() const noexcept {
  return mActive < mLayouts.size() ? mLayouts[mActive].get() : nullptr;
}

bool Keyboard::HasAltGr() const noexcept {
  const auto* layout = ActiveLayout();
  return layout && layout->HasAltGr();
}

void Keyboard::Rebuild() {
  ReleaseAll();
  const auto* layout = ActiveLayout();
  mTypist.reset();
  if (layout) {
    mTypist.emplace(mLayouts[mActive]);
  }
  const GeometryOptions options {.mISO = layout && layout->IsISO(), .mFunctionRow = mFunctionRow};
  if (!mGeometry || !(mGeometry->mOptions == options)) {
    mGeometry = std::make_shared<const Geometry>(BuildGeometry(options));
  }
}

KeyboardLayout::ModifierState Keyboard::LayoutState() const noexcept {
  const bool altGr = HasAltGr() && (IsOn(Modifier::AltGr) || (IsOn(Modifier::Control) && IsOn(Modifier::Alt)));
  return {.mShift = IsOn(Modifier::Shift), .mAltGr = altGr, .mCapsLock = mCapsLock};
}

void Keyboard::TapModifier(Modifier modifier, double now) {
  const auto index = static_cast<std::size_t>(modifier);
  auto& state = mModifiers[index];
  switch (state) {
    case ModifierState::Off:
      state = ModifierState::Latched;
      mLatchedAt[index] = now;
      break;
    case ModifierState::Latched:
      state = (now - mLatchedAt[index] <= RepeatTiming::LockTime) ? ModifierState::Locked : ModifierState::Off;
      break;
    case ModifierState::Locked:
      state = ModifierState::Off;
      break;
  }
}

std::optional<wchar_t> Keyboard::PendingDeadKey() const {
  return mTypist ? mTypist->GetPendingDeadKey() : std::nullopt;
}

bool Keyboard::IsHeld(int key) const noexcept {
  return std::ranges::find(mHeld, key) != mHeld.end();
}

PressResult Keyboard::Press(int key, double now) {
  PressResult result;
  if (!mGeometry || key < 0 || static_cast<std::size_t>(key) >= mGeometry->mKeys.size()) {
    return result;
  }
  const auto& k = mGeometry->mKeys[static_cast<std::size_t>(key)];
  switch (k.mRole) {
    case KeyRole::Shift:
      TapModifier(Modifier::Shift, now);
      result.mChanged = true;
      return result;
    case KeyRole::Control:
      TapModifier(Modifier::Control, now);
      result.mChanged = true;
      return result;
    case KeyRole::Alt:
      TapModifier(Modifier::Alt, now);
      result.mChanged = true;
      return result;
    case KeyRole::AltGr:
      TapModifier(HasAltGr() ? Modifier::AltGr : Modifier::Alt, now);
      result.mChanged = true;
      return result;
    case KeyRole::CapsLock:
      mCapsLock = !mCapsLock;
      result.mChanged = true;
      return result;
    case KeyRole::NumLock:
      mNumLock = !mNumLock;
      result.mChanged = true;
      return result;
    case KeyRole::Layout:
      if (mLayouts.size() > 1) {
        mActive = (mActive + 1) % mLayouts.size();
        Rebuild();
        result.mLayoutChanged = true;
        result.mChanged = true;
      }
      return result;
    case KeyRole::Hide:
      result.mHide = true;
      return result;
    case KeyRole::Character:
    case KeyRole::Action:
    case KeyRole::Numpad:
      break;
  }

  const auto* layout = ActiveLayout();
  const bool shift = IsOn(Modifier::Shift);
  const bool control = IsOn(Modifier::Control);
  const bool alt = IsOn(Modifier::Alt);
  const auto state = LayoutState();
  // AltGr is Ctrl+Alt to the app (doc 06)
  const bool altGr = state.mAltGr;
  const bool controlOnly = control && !alt && !altGr;
  const bool altOnly = alt && !control && !altGr;

  VirtualKeyStroke stroke;
  stroke.mScanCode = k.mScanCode;
  stroke.mShift = shift;
  stroke.mControl = control || altGr;
  stroke.mAlt = alt || altGr;
  stroke.mCapsLock = mCapsLock;
  stroke.mNumLock = mNumLock;
  if (layout) {
    stroke.mVirtualKey = (k.mRole == KeyRole::Numpad && mNumLock) ? layout->GetNumLockVirtualKey(k.mScanCode)
                                                                  : layout->GetVirtualKey(k.mScanCode);
  }

  std::wstring repeatText;
  bool deadKeyAlone = false;
  if (k.mRole == KeyRole::Action) {
    if (mTypist) {
      mTypist->CancelDeadKey();
    }
    switch (k.mScanCode) {
      case 0x1C:
      case 0xE01C:
        result.mEnter = true;
        stroke.mText = controlOnly ? L"\n" : L"\r";
        break;
      case 0x0E:
        stroke.mText = controlOnly ? std::wstring(1, L'\x7F') : std::wstring(1, L'\b');
        break;
      case 0x0F:
        stroke.mText = (control || alt) ? L"" : L"\t";
        break;
      case 0x01:
        stroke.mText = altOnly ? L"" : std::wstring(1, L'\x1B');
        break;
      default:
        break;
    }
    repeatText = stroke.mText;
  } else if (k.mRole == KeyRole::Numpad) {
    if (layout && mNumLock && !control && !alt) {
      const auto output = layout->GetOutput(k.mScanCode, {});
      if (output.mKind == KeyOutput::Kind::Text) {
        stroke.mText = output.mText;
      }
    }
    repeatText = stroke.mText;
  } else if (controlOnly) {
    // TranslateMessage's control characters: Ctrl+A is 0x01 ... Ctrl+Z 0x1A
    if (stroke.mVirtualKey >= 'A' && stroke.mVirtualKey <= 'Z') {
      stroke.mText = std::wstring(1, static_cast<wchar_t>(stroke.mVirtualKey - 'A' + 1));
    }
    repeatText = stroke.mText;
  } else if (!altOnly && layout && mTypist) {
    const auto output = layout->GetOutput(k.mScanCode, state);
    const auto typed = mTypist->Press(k.mScanCode, state);
    stroke.mText = typed.mText;
    deadKeyAlone = typed.mText.empty() && typed.mPendingDeadKey.has_value()
      && output.mKind == KeyOutput::Kind::DeadKey;
    // A held key repeats what it types on its own (Windows: the accent
    // went with the first one)
    if (output.mKind == KeyOutput::Kind::Text) {
      repeatText = output.mText;
    }
    if (deadKeyAlone || typed.mPendingDeadKey || !typed.mText.empty()) {
      result.mChanged = true;// the pending accent shows, or stops showing
    }
  }

  // A latched modifier lasts for one key
  for (auto& modifier: mModifiers) {
    if (modifier == ModifierState::Latched) {
      modifier = ModifierState::Off;
      result.mChanged = true;
    }
  }
  if (deadKeyAlone) {
    return result;// sent with the next key
  }
  if (!IsHeld(key)) {
    mHeld.push_back(key);
  }
  if (k.mRepeats) {
    mRepeatKey = key;
    mRepeatStroke = stroke;
    mRepeatStroke.mText = repeatText;
    mRepeatStroke.mRepeat = true;
    mNextRepeat = now + mRepeatTiming.mDelaySeconds;
  } else {
    mRepeatKey = -1;
  }
  result.mStroke = std::move(stroke);
  return result;
}

void Keyboard::Release(int key) {
  std::erase(mHeld, key);
  if (key == mRepeatKey) {
    mRepeatKey = -1;
  }
}

std::optional<VirtualKeyStroke> Keyboard::Repeat(double now) {
  if (mRepeatKey < 0 || now < mNextRepeat) {
    return std::nullopt;
  }
  const double interval = std::max(RepeatTiming::MinimumInterval, mRepeatTiming.mIntervalSeconds);
  // Behind by more than a repeat (the thread was late): carry on from now,
  // never catch up with a burst
  mNextRepeat = (now - mNextRepeat >= interval) ? now + interval : mNextRepeat + interval;
  return mRepeatStroke;
}

void Keyboard::ReleaseAll() noexcept {
  mHeld.clear();
  mRepeatKey = -1;
}

void Keyboard::ClearModifiers() noexcept {
  mModifiers = {};
  if (mTypist) {
    mTypist->CancelDeadKey();
  }
}

Legend Keyboard::GetLegend(int key) const {
  Legend legend;
  if (!mGeometry || key < 0 || static_cast<std::size_t>(key) >= mGeometry->mKeys.size()) {
    return legend;
  }
  const auto& k = mGeometry->mKeys[static_cast<std::size_t>(key)];
  const auto* layout = ActiveLayout();
  if (k.mRole == KeyRole::AltGr) {
    legend.mText = HasAltGr() ? L"AltGr" : L"Alt";
    return legend;
  }
  if (k.mRole == KeyRole::Numpad) {
    if (layout) {
      legend.mText = layout->GetOutput(k.mScanCode, {}).mText;
    }
    legend.mSmall = k.mSecondLabel;
    return legend;
  }
  if (k.mRole != KeyRole::Character || !layout) {
    legend.mText = k.mLabel;
    return legend;
  }
  if (k.mScanCode == 0x39) {
    legend.mText = layout->GetInfo().mDisplayName;// the space bar names the layout
    return legend;
  }
  const auto state = LayoutState();
  auto output = layout->GetOutput(k.mScanCode, state);
  if (output.mKind == KeyOutput::Kind::None && (state.mShift || state.mAltGr)) {
    output = layout->GetOutput(k.mScanCode, {.mCapsLock = mCapsLock});
  }
  if (output.mKind == KeyOutput::Kind::DeadKey && !output.mText.empty()) {
    legend.mText = std::wstring(1, layout->GetSpacingDeadChar(output.mText.front()));
    legend.mDead = true;
  } else {
    legend.mText = output.mText;
  }
  // The AltGr character in a corner, while AltGr isn't on
  if (layout->HasAltGr() && !state.mAltGr) {
    const auto altGr = layout->GetOutput(k.mScanCode, {.mShift = state.mShift, .mAltGr = true, .mCapsLock = mCapsLock});
    if (altGr.mKind == KeyOutput::Kind::Text) {
      legend.mSmall = altGr.mText;
    } else if (altGr.mKind == KeyOutput::Kind::DeadKey && !altGr.mText.empty()) {
      legend.mSmall = std::wstring(1, layout->GetSpacingDeadChar(altGr.mText.front()));
    }
  }
  return legend;
}

}// namespace OpenDesktopVR::VirtualKeyboard
