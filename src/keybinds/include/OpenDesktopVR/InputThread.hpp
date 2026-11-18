// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The thread that watches the keyboard and game controllers for keybinds,
// while the game has focus (doc 09 "Input APIs"):
//
// - Keyboard: Raw Input with RIDEV_INPUTSINK on a hidden window, never a
//   low-level hook (KY-3). Keys are only matched against bindings: never
//   logged, never kept.
// - Joysticks, HOTAS, button boxes: DirectInput 8, background and
//   non-exclusive. Each device signals an event when it changes (no
//   polling, except devices that need it); a check once a second notices a
//   lost device.
// - A lost device (unplugged, sleep, the lock screen, a UAC prompt) is
//   re-acquired with a back-off from 100 ms to 5 s that resets only after a
//   successful read, and read at once when it's back, so buttons let go
//   meanwhile read as up and fire nothing (KY-2).
// - WM_DEVICECHANGE is debounced by 250 ms; the device list is then
//   enumerated again, on this thread, and held buttons are forgotten
//   (KY-1). Bindings name devices; they never own them.
// - A binding that fires calls `onFire` on this thread: hand it off (post a
//   message), don't work in it.
// - Bind capture (KY-4): a shared, locked session that outlives the dialog;
//   while one is open, no binding fires.
#pragma once

#include <OpenDesktopVR/BindCapture.hpp>
#include <OpenDesktopVR/ChordTracker.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace OpenDesktopVR::Keybinds {

// Re-acquiring a lost device: 100 ms, doubling to 5 s; back to 100 ms only
// after a read succeeds, so a device that acquires but can't be read never
// spins (KY-2)
class Backoff {
 public:
  static constexpr std::chrono::milliseconds First {100};
  static constexpr std::chrono::milliseconds Longest {5000};
  // The delay before the next try; each call doubles the following one
  std::chrono::milliseconds Next() noexcept {
    const auto delay = mDelay;
    mDelay = std::min(mDelay * 2, Longest);
    return delay;
  }
  void Succeeded() noexcept {
    mDelay = First;
  }

 private:
  std::chrono::milliseconds mDelay {First};
};

class CaptureSession {
 public:
  BindCapture::State State() const {
    std::scoped_lock lock(mMutex);
    return mCapture.GetState();
  }
  Binding Current() const {
    std::scoped_lock lock(mMutex);
    return mCapture.Current();
  }
  bool ModifierOnly() const {
    std::scoped_lock lock(mMutex);
    return mCapture.ModifierOnly();
  }
  void Cancel() {
    std::scoped_lock lock(mMutex);
    mCapture.Cancel();
  }
  // The input thread: what's held when it first sees the session, then
  // each button
  void Prime(std::vector<Button> held) {
    std::scoped_lock lock(mMutex);
    if (!mPrimed) {
      mPrimed = true;
      mCapture.Ignore(std::move(held));
    }
  }
  void OnButton(const Button& button, bool pressed) {
    std::scoped_lock lock(mMutex);
    mCapture.OnButton(button, pressed);
  }

 private:
  mutable std::mutex mMutex;
  BindCapture mCapture;
  bool mPrimed {false};
};

struct ControllerInfo {
  std::string mDevice; // instance GUID
  std::string mProduct;// product GUID
  std::string mName;
};

class InputThread {
 public:
  using FireHandler = std::function<void(const Action&)>;

  explicit InputThread(FireHandler onFire);
  ~InputThread();
  InputThread(const InputThread&) = delete;
  InputThread& operator=(const InputThread&) = delete;

  // Any thread. Controllers are found again by product if their instance
  // changed (another USB port), when only one of that product is connected.
  void SetBindings(std::vector<BoundAction> bindings);

  // Any thread. Ends any capture already open.
  std::shared_ptr<CaptureSession> StartCapture();

  // Connected game controllers, for the settings window
  std::vector<ControllerInfo> Controllers() const;
  // Raw Input registered (keyboard keybinds work)
  bool KeyboardReady() const noexcept {
    return mKeyboardReady.load();
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
  std::atomic<bool> mKeyboardReady {false};
};

}// namespace OpenDesktopVR::Keybinds
