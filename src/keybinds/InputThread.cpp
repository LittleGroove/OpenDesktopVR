// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/ControllerState.hpp>
#include <OpenDesktopVR/InputThread.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>

#include <dinput.h>
#include <wtsapi32.h>

#include <winrt/base.h>

#include <algorithm>
#include <format>

namespace OpenDesktopVR::Keybinds {

using namespace std::chrono_literals;

namespace {

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.Input";
constexpr UINT_PTR DeviceChangeTimer = 1;
constexpr UINT DeviceChangeDebounceMs = 250;
constexpr auto CheckInterval = 1s;          // notices a lost device
constexpr auto PolledInterval = 10ms;       // devices that send no events
constexpr std::size_t MaxControllers = 32;  // events in one wait, with room
constexpr auto MissLogInterval = 5s;        // "didn't fire" lines, at most

// "button 5, hat 1 up": the device's name is said once, before
std::string CodeNames(const std::vector<uint32_t>& codes) {
  std::string names;
  for (const auto code: codes) {
    names += (names.empty() ? "" : ", ") + ControllerCodeName(code);
  }
  return names;
}

std::string GuidText(const GUID& guid) {
  wchar_t text[64] {};
  StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
  return StringUtil::Narrow(text);
}

// Raw Input's key: left and right modifiers as one, E0 kept for the rest
std::optional<uint32_t> KeyCode(const RAWKEYBOARD& key) {
  UINT vk = key.VKey;
  if (vk == 0 || vk >= 0xFF) {
    return std::nullopt;// fake keys (Pause's prefix, a missing mapping)
  }
  bool extended = (key.Flags & RI_KEY_E0) != 0;
  switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
      vk = VK_SHIFT;
      extended = false;
      break;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
      vk = VK_CONTROL;
      extended = false;
      break;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
      vk = VK_MENU;
      extended = false;
      break;
    case VK_LWIN:
    case VK_RWIN:
      vk = VK_LWIN;
      extended = false;
      break;
    default:
      break;
  }
  return vk | (extended ? ExtendedKey : 0);
}

// A hat's direction(s) from its angle (hundredths of a degree; centred is
// 0xFFFF in the low word). Diagonals hold two.
void HatCodes(uint32_t hat, DWORD angle, std::vector<uint32_t>& codes) {
  if (LOWORD(angle) == 0xFFFF || angle >= 36000) {
    return;
  }
  if (angle >= 31500 || angle <= 4500) {
    codes.push_back(HatCode(hat, HatDirection::Up));
  }
  if (angle >= 4500 && angle <= 13500) {
    codes.push_back(HatCode(hat, HatDirection::Right));
  }
  if (angle >= 13500 && angle <= 22500) {
    codes.push_back(HatCode(hat, HatDirection::Down));
  }
  if (angle >= 22500 && angle <= 31500) {
    codes.push_back(HatCode(hat, HatDirection::Left));
  }
}

}// namespace

struct InputThread::Impl {
  struct Controller {
    ControllerInfo mInfo;
    winrt::com_ptr<IDirectInputDevice8W> mDevice;
    HANDLE mEvent {nullptr};// null: the device must be polled
    uint32_t mButtons {0};
    uint32_t mHats {0};
    bool mAcquired {false};
    bool mLoggedLoss {false};
    Backoff mBackoff;
    TimeUtil::TimePoint mNextAttempt {};
    ControllerState mState;
    std::vector<uint32_t> mLoggedOn;// last logged "on from the start"

    ~Controller() {
      if (mDevice) {
        mDevice->Unacquire();
        mDevice->SetEventNotification(nullptr);
      }
      if (mEvent) {
        CloseHandle(mEvent);
      }
    }
  };

  Impl(FireHandler onFire, std::atomic<bool>& keyboardReady) : mOnFire(std::move(onFire)), mKeyboardReady(keyboardReady) {
  }
  ~Impl() {
    CloseHandle(mWake);
  }

  FireHandler mOnFire;
  std::atomic<bool>& mKeyboardReady;
  std::thread mThread;
  HANDLE mWake {CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  std::atomic<bool> mStop {false};

  // Shared with other threads
  mutable std::mutex mMutex;
  std::optional<std::vector<BoundAction>> mPendingBindings;
  std::shared_ptr<CaptureSession> mCapture;
  std::vector<ControllerInfo> mControllerInfo;

  // The input thread's own
  HWND mWindow {nullptr};
  winrt::com_ptr<IDirectInput8W> mDirectInput;
  std::vector<std::unique_ptr<Controller>> mControllers;
  std::vector<BoundAction> mBindings;
  ChordTracker mTracker;
  TimeUtil::TimePoint mNextCheck {};
  bool mEnumerated {false};
  TimeUtil::RateLimiter mMissLog {MissLogInterval};

  void Main();
  bool CreateWindowAndInputs();
  void Loop();
  void ApplyPending();
  void Resolve();
  void Enumerate();
  std::unique_ptr<Controller> OpenController(const DIDEVICEINSTANCEW& instance);
  void Read(Controller& controller);
  void Lost(Controller& controller);
  void TryAcquire(Controller& controller);
  void TimedWork();
  void Feed(const Button& button, bool pressed);
  void OnRawInput(LPARAM lParam);
  void ForgetKeyboard();
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
};

InputThread::InputThread(FireHandler onFire) : mImpl(std::make_unique<Impl>(std::move(onFire), mKeyboardReady)) {
  mImpl->mThread = std::thread([impl = mImpl.get()] {
    ThreadUtil::SetCurrentThreadName(L"Keybinds");
    ThreadUtil::CatchAll("keybind input thread", [impl] { impl->Main(); });
  });
}

InputThread::~InputThread() {
  mImpl->mStop = true;
  SetEvent(mImpl->mWake);
  if (mImpl->mThread.joinable()) {
    mImpl->mThread.join();
  }
}

void InputThread::SetBindings(std::vector<BoundAction> bindings) {
  {
    std::scoped_lock lock(mImpl->mMutex);
    mImpl->mPendingBindings = std::move(bindings);
  }
  SetEvent(mImpl->mWake);
}

std::shared_ptr<CaptureSession> InputThread::StartCapture() {
  auto session = std::make_shared<CaptureSession>();
  std::scoped_lock lock(mImpl->mMutex);
  if (mImpl->mCapture) {
    mImpl->mCapture->Cancel();
  }
  mImpl->mCapture = session;
  return session;
}

std::vector<ControllerInfo> InputThread::Controllers() const {
  std::scoped_lock lock(mImpl->mMutex);
  return mImpl->mControllerInfo;
}

void InputThread::Impl::Main() {
  if (!CreateWindowAndInputs()) {
    return;
  }
  Loop();
  mControllers.clear();
  mDirectInput = nullptr;
  WTSUnRegisterSessionNotification(mWindow);
  DestroyWindow(mWindow);
  UnregisterClassW(WindowClass, GetModuleHandleW(nullptr));
}

bool InputThread::Impl::CreateWindowAndInputs() {
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  // A hidden top-level window: it gets device-change broadcasts, which
  // message-only windows don't
  mWindow = CreateWindowExW(0, WindowClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
  if (!mWindow) {
    Log::Error("Keybinds: no input window ({}); keybinds are off", GetLastError());
    return false;
  }
  const RAWINPUTDEVICE keyboard {0x01, 0x06, RIDEV_INPUTSINK, mWindow};// generic desktop, keyboard
  if (RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard))) {
    mKeyboardReady = true;
  } else {
    Log::Error("Keybinds: the keyboard can't be watched ({}); keyboard keybinds are off", GetLastError());
  }
  WTSRegisterSessionNotification(mWindow, NOTIFY_FOR_THIS_SESSION);
  if (FAILED(DirectInput8Create(instance, DIRECTINPUT_VERSION, IID_IDirectInput8W, mDirectInput.put_void(), nullptr))) {
    Log::Error("Keybinds: DirectInput isn't available; game controller keybinds are off");
  }
  Enumerate();
  return true;
}

void InputThread::Impl::Loop() {
  std::vector<HANDLE> handles;
  std::vector<Controller*> owners;
  while (!mStop) {
    handles.assign(1, mWake);
    owners.assign(1, nullptr);
    bool anyPolled = false;
    for (auto& controller: mControllers) {
      if (controller->mAcquired && controller->mEvent) {
        handles.push_back(controller->mEvent);
        owners.push_back(controller.get());
      }
      anyPolled |= controller->mAcquired && !controller->mEvent;
    }
    // Until the next timed job: a check, a re-acquire, a polled device
    auto due = mNextCheck;
    for (const auto& controller: mControllers) {
      if (!controller->mAcquired) {
        due = std::min(due, controller->mNextAttempt);
      } else if (const auto settled = controller->mState.SettleDue()) {
        due = std::min(due, *settled);
      }
    }
    if (anyPolled) {
      due = std::min(due, TimeUtil::Now() + PolledInterval);
    }
    const auto wait = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(due - TimeUtil::Now()).count());
    const auto woke = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(),
      static_cast<DWORD>(std::min<int64_t>(wait, 1000)), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (mStop) {
      break;
    }
    if (woke == WAIT_OBJECT_0) {
      ApplyPending();
    } else if (woke > WAIT_OBJECT_0 && woke < WAIT_OBJECT_0 + handles.size()) {
      Read(*owners[woke - WAIT_OBJECT_0]);
    } else if (woke == WAIT_OBJECT_0 + handles.size()) {
      MSG message {};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        DispatchMessageW(&message);
      }
    }
    TimedWork();
  }
}

void InputThread::Impl::TimedWork() {
  const auto now = TimeUtil::Now();
  for (auto& controller: mControllers) {
    if (!controller->mAcquired && now >= controller->mNextAttempt) {
      TryAcquire(*controller);
    } else if (controller->mAcquired && !controller->mEvent) {
      Read(*controller);
    } else if (const auto settled = controller->mState.SettleDue(); controller->mAcquired && settled && now >= *settled) {
      Read(*controller);// settling is over: what's held now is the baseline
    }
  }
  if (now >= mNextCheck) {
    mNextCheck = now + CheckInterval;
    for (auto& controller: mControllers) {
      if (controller->mAcquired && controller->mEvent) {
        Read(*controller);
      }
    }
  }
}

void InputThread::Impl::ApplyPending() {
  std::optional<std::vector<BoundAction>> bindings;
  {
    std::scoped_lock lock(mMutex);
    bindings = std::exchange(mPendingBindings, std::nullopt);
  }
  if (bindings) {
    mBindings = std::move(*bindings);
    Resolve();
  }
}

void InputThread::Impl::Resolve() {
  // A controller on another USB port may get a new instance: match it by
  // product when only one of that product is connected
  auto resolved = mBindings;
  for (auto& bound: resolved) {
    for (auto& button: bound.mBinding.mButtons) {
      if (button.mKind != DeviceKind::Controller) {
        continue;
      }
      const bool connected = std::ranges::any_of(mControllers, [&](const auto& c) { return c->mInfo.mDevice == button.mDevice; });
      if (connected || button.mProduct.empty()) {
        continue;
      }
      const Controller* only = nullptr;
      int count = 0;
      for (const auto& c: mControllers) {
        if (c->mInfo.mProduct == button.mProduct) {
          only = c.get();
          ++count;
        }
      }
      if (count == 1) {
        button.mDevice = only->mInfo.mDevice;
      }
    }
    bound.mBinding = Binding::From(std::move(bound.mBinding.mButtons));
  }
  mTracker.SetBindings(std::move(resolved));
}

std::unique_ptr<InputThread::Impl::Controller> InputThread::Impl::OpenController(const DIDEVICEINSTANCEW& instance) {
  auto controller = std::make_unique<Controller>();
  controller->mInfo = {GuidText(instance.guidInstance), GuidText(instance.guidProduct), StringUtil::Narrow(instance.tszProductName)};
  if (FAILED(mDirectInput->CreateDevice(instance.guidInstance, controller->mDevice.put(), nullptr))
      || FAILED(controller->mDevice->SetDataFormat(&c_dfDIJoystick2))
      || FAILED(controller->mDevice->SetCooperativeLevel(mWindow, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
    Log::Warning("Keybinds: can't use '{}'", controller->mInfo.mName);
    return nullptr;
  }
  DIDEVCAPS caps {sizeof(caps)};
  if (SUCCEEDED(controller->mDevice->GetCapabilities(&caps))) {
    controller->mButtons = std::min<DWORD>(caps.dwButtons, 128);
    controller->mHats = std::min<DWORD>(caps.dwPOVs, 4);
  }
  // Signalled on every change; devices that can't, are polled
  controller->mEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (controller->mEvent && controller->mDevice->SetEventNotification(controller->mEvent) != DI_OK) {
    CloseHandle(controller->mEvent);
    controller->mEvent = nullptr;
  }
  TryAcquire(*controller);
  return controller;
}

void InputThread::Impl::Enumerate() {
  if (!mDirectInput) {
    return;
  }
  std::vector<DIDEVICEINSTANCEW> found;
  mDirectInput->EnumDevices(
    DI8DEVCLASS_GAMECTRL,
    [](const DIDEVICEINSTANCEW* instance, void* context) -> BOOL {
      static_cast<std::vector<DIDEVICEINSTANCEW>*>(context)->push_back(*instance);
      return DIENUM_CONTINUE;
    },
    &found,
    DIEDFL_ATTACHEDONLY);
  if (found.size() > MaxControllers) {
    found.resize(MaxControllers);
  }
  std::vector<std::unique_ptr<Controller>> next;
  bool changed = false;
  for (const auto& instance: found) {
    const auto id = GuidText(instance.guidInstance);
    auto existing = std::ranges::find_if(mControllers, [&](const auto& c) { return c && c->mInfo.mDevice == id; });
    if (existing != mControllers.end()) {
      next.push_back(std::move(*existing));
    } else if (auto opened = OpenController(instance)) {
      next.push_back(std::move(opened));
      changed = true;
    }
  }
  changed |= std::ranges::any_of(mControllers, [](const auto& c) { return c != nullptr; });// some went away
  changed |= !std::exchange(mEnumerated, true);// the first time: say what's there
  mControllers = std::move(next);
  if (!changed) {
    return;
  }
  // Held buttons are forgotten when devices change (KY-1); the devices
  // still here are read again at once, as a baseline
  mTracker.Clear();
  Resolve();
  for (auto& controller: mControllers) {
    controller->mState.Resync();
    if (controller->mAcquired) {
      Read(*controller);
    }
  }
  std::vector<ControllerInfo> info;
  std::string names;
  for (const auto& controller: mControllers) {
    info.push_back(controller->mInfo);
    names += (names.empty() ? "" : ", ") + controller->mInfo.mName;
  }
  {
    std::scoped_lock lock(mMutex);
    mControllerInfo = std::move(info);
  }
  Log::Info("Keybinds: {} game controller{}{}{}", mControllers.size(), mControllers.size() == 1 ? "" : "s",
    names.empty() ? "" : ": ", names);
}

void InputThread::Impl::TryAcquire(Controller& controller) {
  if (SUCCEEDED(controller.mDevice->Acquire())) {
    controller.mAcquired = true;
    // At once: what was let go meanwhile reads as up (KY-2). Its reports
    // are a baseline until it settles (test round 8).
    controller.mState.Restart(TimeUtil::Now());
    Read(controller);
    return;
  }
  controller.mNextAttempt = TimeUtil::Now() + controller.mBackoff.Next();
}

void InputThread::Impl::Lost(Controller& controller) {
  controller.mAcquired = false;
  controller.mNextAttempt = TimeUtil::Now() + controller.mBackoff.Next();
  if (!controller.mLoggedLoss) {
    controller.mLoggedLoss = true;
    Log::Info("Keybinds: '{}' was lost (unplugged, sleep or the lock screen); trying again", controller.mInfo.mName);
  }
}

void InputThread::Impl::Read(Controller& controller) {
  controller.mDevice->Poll();// needed by polled devices; harmless for the rest
  DIJOYSTATE2 state {};
  if (const auto result = controller.mDevice->GetDeviceState(sizeof(state), &state); FAILED(result)) {
    if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED) {
      Lost(controller);
    }
    return;
  }
  controller.mBackoff.Succeeded();
  if (controller.mLoggedLoss) {
    controller.mLoggedLoss = false;
    Log::Info("Keybinds: '{}' is back", controller.mInfo.mName);
  }
  std::vector<uint32_t> held;
  for (uint32_t i = 0; i < controller.mButtons; ++i) {
    if (state.rgbButtons[i] & 0x80) {
      held.push_back(i);
    }
  }
  for (uint32_t hat = 0; hat < controller.mHats; ++hat) {
    HatCodes(hat, state.rgdwPOV[hat], held);
  }
  std::ranges::sort(held);
  held.erase(std::ranges::unique(held).begin(), held.end());
  const auto changes = controller.mState.Update(std::move(held), TimeUtil::Now());
  if (changes.mBaseline) {
    // New, back after a loss, or the device list changed: what's held now,
    // silently
    const auto& on = controller.mState.Held();
    mTracker.Resync(DeviceKind::Controller, controller.mInfo.mDevice, on);
    // Switches and detents: on from the start, so they don't block keybinds.
    // Said once per change (test round 7: a HOTAS whose bindings never fired).
    if (changes.mSettled) {
      if (!on.empty() && on != controller.mLoggedOn) {
        Log::Info("Keybinds: '{}' has {} on from the start (switches or detents; they don't block keybinds): {}",
          controller.mInfo.mName, on.size() == 1 ? "1 button" : std::format("{} buttons", on.size()), CodeNames(on));
      }
      controller.mLoggedOn = on;
    }
    return;
  }
  const auto button = [&](uint32_t code) {
    return Button {DeviceKind::Controller, controller.mInfo.mDevice, code, controller.mInfo.mProduct, controller.mInfo.mName};
  };
  for (const auto code: changes.mReleased) {
    Feed(button(code), false);
  }
  for (const auto code: changes.mPressed) {
    Feed(button(code), true);
  }
}

void InputThread::Impl::Feed(const Button& button, bool pressed) {
  std::shared_ptr<CaptureSession> capture;
  {
    std::scoped_lock lock(mMutex);
    capture = mCapture;
  }
  if (capture) {
    capture->Prime(mTracker.Held());// before this button counts as held
  }
  const auto action = mTracker.OnButton(button, pressed);
  // A controller binding that didn't fire because of other buttons pressed
  // with it: say which, so a report can tell why (never keyboard keys)
  if (auto miss = mTracker.TakeMiss(); miss && !capture && mMissLog.Allow()) {
    std::string also;
    for (const auto& extra: miss->mAlsoPressed) {
      also += (also.empty() ? "" : ", ") + ButtonName(extra);
    }
    Log::Info("Keybinds: {} didn't fire: also pressed with it: {}", BindingName(miss->mBinding), also);
  }
  if (capture) {
    const auto state = capture->State();
    if (state == BindCapture::State::Waiting || state == BindCapture::State::Holding) {
      capture->OnButton(button, pressed);
      return;// no binding fires while one is being set
    }
    std::scoped_lock lock(mMutex);
    if (mCapture == capture) {
      mCapture.reset();// done or cancelled
    }
  }
  if (action) {
    ThreadUtil::CatchAll("keybind action", [&] { mOnFire(*action); });
  }
}

void InputThread::Impl::OnRawInput(LPARAM lParam) {
  RAWINPUT input {};
  UINT size = sizeof(input);
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)
      || input.header.dwType != RIM_TYPEKEYBOARD) {
    return;
  }
  if (const auto code = KeyCode(input.data.keyboard)) {
    Feed(Button {DeviceKind::Keyboard, {}, *code}, (input.data.keyboard.Flags & RI_KEY_BREAK) == 0);
  }
}

void InputThread::Impl::ForgetKeyboard() {
  mTracker.DropKeys([](uint32_t) { return true; });
}

LRESULT CALLBACK InputThread::Impl::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
  }
  auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self) {
    LRESULT result = 0;
    bool handled = false;
    ThreadUtil::CatchAll("keybind window", [&] {
      switch (message) {
        case WM_INPUT:
          self->OnRawInput(lParam);
          break;// DefWindowProc cleans up after WM_INPUT
        case WM_DEVICECHANGE:
          // Several arrive per change, for any device: settle first
          SetTimer(window, DeviceChangeTimer, DeviceChangeDebounceMs, nullptr);
          result = TRUE;
          handled = true;
          break;
        case WM_TIMER:
          if (wParam == DeviceChangeTimer) {
            KillTimer(window, DeviceChangeTimer);
            self->Enumerate();
            handled = true;
          }
          break;
        case WM_WTSSESSION_CHANGE:
          // Keys let go on the lock screen never arrive here
          if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT) {
            self->ForgetKeyboard();
          }
          break;
        case WM_POWERBROADCAST:
          if (wParam == PBT_APMRESUMEAUTOMATIC) {
            self->ForgetKeyboard();
          }
          break;
        default:
          break;
      }
    });
    if (handled) {
      return result;
    }
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}// namespace OpenDesktopVR::Keybinds
