// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/KeyboardAutoShow.hpp>
#include <OpenDesktopVR/KeyboardService.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/TextInputMonitor.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>
#include <OpenDesktopVR/VirtualKeyboard.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>

namespace OpenDesktopVR::Host {

using namespace std::chrono_literals;
using HandInteraction::TypingFingerState;
using VirtualKeyboard::AutoShow;
using VirtualKeyboard::KeyFace;
using VirtualKeyboard::KeyRole;
using VirtualKeyboard::Modifier;
using VirtualKeyboard::ModifierState;

RECT CaretInContent(const Pointer::Target& target, const RECT& caretClient, LONG clientWidth, LONG clientHeight) noexcept {
  // The window's own units to the captured frame's pixels, then to the
  // shown part's
  const double sx = clientWidth > 0 ? static_cast<double>(target.mClient.cx) / clientWidth : 1.0;
  const double sy = clientHeight > 0 ? static_cast<double>(target.mClient.cy) / clientHeight : 1.0;
  const auto x = [&](LONG value) { return static_cast<LONG>(std::lround((value * sx) - target.mShown.left)); };
  const auto y = [&](LONG value) { return static_cast<LONG>(std::lround((value * sy) - target.mShown.top)); };
  return {x(caretClient.left), y(caretClient.top), x(caretClient.right), y(caretClient.bottom)};
}

namespace {

double SecondsNow() {
  return std::chrono::duration<double>(TimeUtil::Now().time_since_epoch()).count();
}

// A hand near the keyboard, or a key typed this recently: "typing" (K9)
constexpr double TypingLinger = 2.0;
// Hover highlight: over the last 4 cm (doc 07)
constexpr float HoverReach = 0.04f;

}// namespace

struct KeyboardService::Impl {
  Pointer::Injector& mInjector;
  std::shared_ptr<VirtualKeyboard::KeyboardModel> mModel;
  std::function<void(float)> mClick;

  mutable std::mutex mMutex;
  // Everything below is guarded by mMutex
  KeyboardSettings mSettings;
  VirtualKeyboard::RepeatTiming mTiming;
  bool mSettingsApplied {false};
  std::vector<std::string> mLayoutIDs;
  std::map<std::string, std::shared_ptr<const KeyboardLayout::Layout>> mLoaded;
  VirtualKeyboard::Keyboard mKeyboard;
  AutoShow mAutoShow;
  HWND mWatched {nullptr};

  // Where keys go: the last finger press on a captured panel
  std::optional<Pointer::Target> mTarget;
  uint64_t mTargetPanel {0};
  std::wstring mTargetName;

  // Typing
  std::array<uint8_t, 2> mBaseline {};
  std::array<int, 2> mFingerKey {-1, -1};
  std::optional<double> mLastPress;
  float mKeyMillimeters {24};

  // Stats (60 s)
  TimeUtil::PeriodicTimer mStatsTimer {60s};
  uint64_t mPresses {0};
  uint64_t mBetweenKeys {0};
  uint64_t mNoTarget {0};
  uint64_t mRepeats {0};
  uint64_t mShows {0};
  double mDistanceSum {0};// key units, presses on keys

  // Last: its callback (its own thread) takes mMutex; destroyed first
  std::optional<TextInputMonitor> mMonitor;

  Impl(Pointer::Injector& injector, std::shared_ptr<VirtualKeyboard::KeyboardModel> model, std::function<void(float)> click)
    : mInjector(injector), mModel(std::move(model)), mClick(std::move(click)) {
    mMonitor.emplace([this](HWND window, const TextInputMonitor::State& state) {
      ThreadUtil::CatchAll("caret monitor", [&] { OnCaret(window, state); });
    });
  }

  ~Impl() {
    mMonitor.reset();// its thread stops before anything it uses goes
  }

  void OnCaret(HWND window, const TextInputMonitor::State& state);
  void Apply(const std::optional<AutoShow::Change>& change);
  void PublishFaces();
  void OnKey(int key, double now);
  void Send(const VirtualKeyStroke& stroke);
  void SetHeld();
  void MaybeLogStats(bool exiting = false);
};

KeyboardService::KeyboardService(
  Pointer::Injector& injector, std::shared_ptr<VirtualKeyboard::KeyboardModel> model, std::function<void(float)> click)
  : mImpl(std::make_unique<Impl>(injector, std::move(model), std::move(click))) {
}

KeyboardService::~KeyboardService() = default;

void KeyboardService::Impl::Apply(const std::optional<AutoShow::Change>& change) {
  if (!change) {
    return;
  }
  if (change->mShown) {
    ++mShows;
    Log::Info("Keyboard shown: {}; typing into '{}'", change->mWhy,
      mTarget ? StringUtil::Narrow(mTargetName) : std::string {"nothing yet"});
  } else {
    Log::Info("Keyboard hidden: {}", change->mWhy);
    // Shown again later, it starts plain
    mKeyboard.ReleaseAll();
    mKeyboard.ClearModifiers();
    mFingerKey = {-1, -1};
    PublishFaces();
  }
  mModel->SetShown(change->mShown);
}

void KeyboardService::Impl::PublishFaces() {
  auto faces = std::make_shared<VirtualKeyboard::KeyboardFaces>();
  faces->mGeometry = mKeyboard.GetGeometry();
  if (!faces->mGeometry) {
    return;
  }
  const auto modifierFlags = [&](Modifier modifier) -> uint8_t {
    switch (mKeyboard.GetModifier(modifier)) {
      case ModifierState::Latched:
        return KeyFace::Latched;
      case ModifierState::Locked:
        return KeyFace::Locked;
      default:
        return 0;
    }
  };
  const auto& keys = faces->mGeometry->mKeys;
  faces->mKeys.resize(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    auto legend = mKeyboard.GetLegend(static_cast<int>(i));
    auto& face = faces->mKeys[i];
    face.mText = std::move(legend.mText);
    face.mSmall = std::move(legend.mSmall);
    face.mFlags = legend.mDead ? KeyFace::Dead : 0;
    switch (keys[i].mRole) {
      case KeyRole::Shift:
        face.mFlags |= modifierFlags(Modifier::Shift);
        break;
      case KeyRole::Control:
        face.mFlags |= modifierFlags(Modifier::Control);
        break;
      case KeyRole::Alt:
        face.mFlags |= modifierFlags(Modifier::Alt);
        break;
      case KeyRole::AltGr:
        face.mFlags |= modifierFlags(mKeyboard.HasAltGr() ? Modifier::AltGr : Modifier::Alt);
        break;
      case KeyRole::CapsLock:
        face.mFlags |= mKeyboard.CapsLock() ? KeyFace::LockOn : 0;
        break;
      case KeyRole::NumLock:
        face.mFlags |= mKeyboard.NumLock() ? KeyFace::LockOn : 0;
        break;
      default:
        break;
    }
  }
  if (const auto* layout = mKeyboard.ActiveLayout()) {
    faces->mLayoutName = layout->GetInfo().mDisplayName;
  }
  faces->mPendingDeadKey = mKeyboard.PendingDeadKey();
  if (faces->mPendingDeadKey && mKeyboard.ActiveLayout()) {
    faces->mPendingDeadKey = mKeyboard.ActiveLayout()->GetSpacingDeadChar(*faces->mPendingDeadKey);
  }
  mModel->SetFaces(std::move(faces));
}

void KeyboardService::SetSettings(const KeyboardSettings& requested) {
  const auto settings = KeyboardConfig::Clean(requested);
  // The layouts: the user's own list, or the one or two chosen (K8)
  std::vector<std::string> ids;
  if (settings.mPrimaryLayout.empty()) {
    ids = KeyboardLayout::GetUserLayoutKLIDs();
  } else {
    ids.push_back(settings.mPrimaryLayout);
    if (!settings.mSecondaryLayout.empty() && settings.mSecondaryLayout != settings.mPrimaryLayout) {
      ids.push_back(settings.mSecondaryLayout);
    }
  }
  // Start on the layout Windows is using here, if it's one of them
  const auto current = KeyboardLayout::GetKLID(GetKeyboardLayout(0));
  const auto timing = VirtualKeyboard::RepeatTiming::FromSystem();

  std::scoped_lock lock(mImpl->mMutex);
  auto& impl = *mImpl;
  // The host passes its settings on every change of any setting
  if (impl.mSettingsApplied && settings == impl.mSettings && ids == impl.mLayoutIDs && timing == impl.mTiming) {
    return;
  }
  impl.mTiming = timing;
  if (ids != impl.mLayoutIDs || !impl.mSettingsApplied) {
    std::vector<VirtualKeyboard::Keyboard::LayoutPtr> layouts;
    std::size_t active = 0;
    for (const auto& id: ids) {
      auto& loaded = impl.mLoaded[id];
      if (!loaded) {
        loaded = KeyboardLayout::Layout::Load(id);// a few ms, here on the UI thread
        if (!loaded) {
          Log::Warning("Keyboard: layout {} couldn't be loaded", id);
          continue;
        }
      }
      if (id == current) {
        active = layouts.size();
      }
      layouts.push_back(loaded);
    }
    std::string names;
    for (const auto& layout: layouts) {
      names += (names.empty() ? "" : ", ") + StringUtil::Narrow(layout->GetInfo().mDisplayName);
    }
    Log::Info("Keyboard layouts: {}", names.empty() ? std::string {"none"} : names);
    impl.mKeyboard.SetLayouts(std::move(layouts), active);
    impl.mLayoutIDs = std::move(ids);
    impl.mFingerKey = {-1, -1};
  }
  impl.mKeyboard.SetFunctionRow(settings.mFunctionRow);
  impl.mKeyboard.SetRepeatTiming(timing);
  impl.mAutoShow.SetHideAfterEnter(settings.mHideAfterEnter);
  impl.mKeyMillimeters = settings.mKeyMillimeters;
  impl.mModel->SetOpacity(settings.mTypingOpacity, settings.mIdleOpacity);
  impl.mSettings = settings;
  impl.mSettingsApplied = true;
  impl.PublishFaces();
  if (!settings.mEnabled) {
    impl.Apply(impl.mAutoShow.Dismiss("the keyboard was turned off"));
  }
}

bool KeyboardService::Toggle() {
  std::scoped_lock lock(mImpl->mMutex);
  if (!mImpl->mSettings.mEnabled) {
    return false;
  }
  mImpl->Apply(mImpl->mAutoShow.Toggle());
  return mImpl->mAutoShow.IsShown();
}

void KeyboardService::Dismiss(const char* why) {
  std::scoped_lock lock(mImpl->mMutex);
  mImpl->Apply(mImpl->mAutoShow.Dismiss(why));
}

bool KeyboardService::IsShown() const {
  std::scoped_lock lock(mImpl->mMutex);
  return mImpl->mAutoShow.IsShown();
}

void KeyboardService::Pressed(const TouchPanel& panel, const Pointer::Target& target, float u, float v) {
  std::scoped_lock lock(mImpl->mMutex);
  auto& impl = *mImpl;
  const bool newTarget = !impl.mTarget || impl.mTarget->mWindow != target.mWindow;
  impl.mTarget = target;
  impl.mTargetPanel = panel.mID;
  impl.mTargetName = panel.mName;
  impl.mModel->SetTarget(panel.mName);
  if (newTarget) {
    impl.mModel->SetPreview(panel.mID, std::nullopt);
  }
  if (!impl.mSettings.mEnabled) {
    return;
  }
  // Where the press was, on the screen (none of these ask the app)
  RECT client {};
  POINT screen {};
  if (GetClientRect(target.mWindow, &client)) {
    screen = Pointer::ClientPoint(target, u, v, client.right, client.bottom);
    ClientToScreen(target.mWindow, &screen);
  }
  impl.mAutoShow.Tapped(SecondsNow(), target.mWindow, screen);
  if (impl.mWatched != target.mWindow) {
    impl.mWatched = target.mWindow;
    impl.mMonitor->SetWindow(target.mWindow);
  }
  for (const auto delay: AutoShow::TapChecks) {
    impl.mMonitor->Poke(delay, true);// VK-2
  }
}

void KeyboardService::Impl::OnCaret(HWND window, const TextInputMonitor::State& state) {
  std::scoped_lock lock(mMutex);
  Apply(mAutoShow.Caret(SecondsNow(), window, state.mTyping, state.mCaret));
  // The preview strip follows the caret in the window keys go to
  if (!mTarget || mTarget->mWindow != window) {
    return;
  }
  std::optional<RECT> caret;
  RECT client {};
  if (state.mTyping && state.mCaret && GetClientRect(window, &client)) {
    POINT topLeft {state.mCaret->left, state.mCaret->top};
    POINT bottomRight {state.mCaret->right, state.mCaret->bottom};
    if (ScreenToClient(window, &topLeft) && ScreenToClient(window, &bottomRight)) {
      caret = CaretInContent(*mTarget, {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y}, client.right, client.bottom);
    }
  }
  mModel->SetPreview(mTargetPanel, caret);
}

void KeyboardService::Typing(const TouchPanel&, const HandInteraction::TypingResult& result, bool appeared) {
  std::scoped_lock lock(mImpl->mMutex);
  auto& impl = *mImpl;
  const double now = SecondsNow();
  const auto& geometry = impl.mKeyboard.GetGeometry();
  if (appeared) {
    // VK-4: whatever the counts are now, they aren't presses
    for (std::size_t h = 0; h < 2; ++h) {
      impl.mBaseline[h] = result.mFingers[h].mPressCount;
    }
    impl.mKeyboard.ReleaseAll();
    impl.mFingerKey = {-1, -1};
    impl.SetHeld();
  }
  std::array<int, 2> hoverKey {-1, -1};
  std::array<uint8_t, 2> hoverLevel {};
  bool nearby = false;
  for (std::size_t h = 0; h < 2; ++h) {
    const auto& finger = result.mFingers[h];
    nearby = nearby || finger.mState != TypingFingerState::NotNear;
    if (finger.mState == TypingFingerState::Pressed && finger.mPressCount != impl.mBaseline[h]) {
      impl.mBaseline[h] = finger.mPressCount;
      // A new press without leaving the key (double letters) lets go first
      if (impl.mFingerKey[h] >= 0) {
        impl.mKeyboard.Release(impl.mFingerKey[h]);
        impl.mFingerKey[h] = -1;
      }
      ++impl.mPresses;
      impl.mLastPress = now;
      const int key = geometry ? VirtualKeyboard::HitTest(*geometry, finger.mU, finger.mV) : -1;
      if (key < 0) {
        ++impl.mBetweenKeys;
        continue;
      }
      impl.mDistanceSum += VirtualKeyboard::DistanceFromCentre(*geometry, key, finger.mU, finger.mV);
      impl.mFingerKey[h] = key;
      impl.OnKey(key, now);
      continue;
    }
    if (finger.mState != TypingFingerState::Pressed) {
      impl.mBaseline[h] = finger.mPressCount;// never a press from a count alone (VK-6)
      if (impl.mFingerKey[h] >= 0) {
        impl.mKeyboard.Release(impl.mFingerKey[h]);
        impl.mFingerKey[h] = -1;
      }
    }
    if (finger.mState == TypingFingerState::Hovering && geometry) {
      const int key = VirtualKeyboard::HitTest(*geometry, finger.mU, finger.mV);
      if (key >= 0) {
        const float closeness = std::clamp(1.0f - (finger.mDistance / HoverReach), 0.0f, 1.0f);
        hoverKey[h] = key;
        hoverLevel[h] = static_cast<uint8_t>(std::clamp(
          static_cast<int>(std::ceil(closeness * VirtualKeyboard::KeyboardFrame::HoverLevels)), 1,
          static_cast<int>(VirtualKeyboard::KeyboardFrame::HoverLevels)));
      }
    }
  }
  impl.SetHeld();
  impl.mModel->SetHover(hoverKey, hoverLevel);
  impl.mModel->SetTyping(nearby || (impl.mLastPress && now - *impl.mLastPress < TypingLinger));
}

void KeyboardService::Impl::OnKey(int key, double now) {
  const auto result = mKeyboard.Press(key, now);
  if (mClick && mSettings.mClickVolume > 0) {
    mClick(mSettings.mClickVolume);
  }
  mModel->Flash(key);
  if (result.mStroke) {
    Send(*result.mStroke);
  }
  if (result.mLayoutChanged) {
    mFingerKey = {-1, -1};// the keys may be others now (ISO and ANSI)
  }
  if (result.mChanged || result.mLayoutChanged) {
    PublishFaces();
  }
  if (result.mEnter) {
    Apply(mAutoShow.Enter(now));
    if (mAutoShow.IsShown() && mWatched) {
      mMonitor->Poke(AutoShow::EnterCheck, true);// focus may move on submit (VK-2)
    }
  }
  if (result.mHide) {
    Apply(mAutoShow.Dismiss("the keyboard's Hide key"));
  }
}

void KeyboardService::Impl::Send(const VirtualKeyStroke& stroke) {
  if (!mTarget || !IsWindow(mTarget->mWindow)) {
    ++mNoTarget;
    mModel->NoTarget();// "Tap a text field first" (doc 07)
    return;
  }
  mInjector.Key(*mTarget, stroke);
}

void KeyboardService::Impl::SetHeld() {
  std::array<int, VirtualKeyboard::KeyboardFrame::MaxHeld> held {-1, -1, -1, -1};
  for (std::size_t h = 0; h < 2; ++h) {
    held[h] = mFingerKey[h];
  }
  mModel->SetHeld(held);
}

void KeyboardService::TypingStopped() {
  std::scoped_lock lock(mImpl->mMutex);
  mImpl->mKeyboard.ReleaseAll();
  mImpl->mFingerKey = {-1, -1};
  mImpl->SetHeld();
  mImpl->mModel->SetHover({-1, -1}, {});
  mImpl->mModel->SetTyping(false);
}

void KeyboardService::Tick() {
  std::scoped_lock lock(mImpl->mMutex);
  auto& impl = *mImpl;
  const double now = SecondsNow();
  if (const auto stroke = impl.mKeyboard.Repeat(now)) {
    ++impl.mRepeats;
    impl.Send(*stroke);
  }
  if (impl.mLastPress && now - *impl.mLastPress >= TypingLinger && impl.mFingerKey == std::array {-1, -1}) {
    impl.mModel->SetTyping(false);
  }
  impl.MaybeLogStats();
}

void KeyboardService::Impl::MaybeLogStats(bool exiting) {
  if (!exiting && !mStatsTimer.Due()) {
    return;
  }
  if (mPresses == 0 && mRepeats == 0 && mShows == 0) {
    return;
  }
  const auto onKeys = mPresses - mBetweenKeys;
  Log::Info(
    "Keyboard ({}): shown {} times; {} presses ({} between keys, {} with nowhere to go), {} repeats; presses landed "
    "{:.1f} mm from key centres on average",
    exiting ? "to exit" : "60 s",
    mShows,
    mPresses,
    mBetweenKeys,
    mNoTarget,
    mRepeats,
    onKeys > 0 ? (mDistanceSum / static_cast<double>(onKeys)) * mKeyMillimeters : 0.0);
  mPresses = mBetweenKeys = mNoTarget = mRepeats = mShows = 0;
  mDistanceSum = 0;
}

void KeyboardService::LogLastStats() {
  std::scoped_lock lock(mImpl->mMutex);
  mImpl->MaybeLogStats(true);
}

}// namespace OpenDesktopVR::Host
