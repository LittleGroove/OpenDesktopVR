// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "KeybindService.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <format>

namespace OpenDesktopVR::Host {

namespace {

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.Keybinds";
constexpr UINT FiredMessage = WM_APP + 20;// wParam: ActionKind; lParam: panel ID
constexpr float BrightnessStep = 0.1f;

std::string PanelName(const PanelManager& panels, uint64_t panelID) {
  const auto panel = panels.Find(panelID);
  return panel ? StringUtil::Narrow(panel->mName) : std::format("{:016x}", panelID);
}

}// namespace

KeybindService::KeybindService(HINSTANCE instance, PanelManager& panels)
  : mInstance(instance),
    mPanels(panels),
    mInput([this](const Keybinds::Action& action) {
      // On the input thread: hand off to the UI thread, never wait
      if (const HWND window = mWindow.load()) {
        PostMessageW(window, FiredMessage, static_cast<WPARAM>(action.mKind), static_cast<LPARAM>(action.mPanelID));
      }
    }) {
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &KeybindService::WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  mWindow = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
  mChanged = mPanels.Changed().Subscribe([this] { Rebuild(); });
  Rebuild();
}

KeybindService::~KeybindService() {
  mChanged.Reset();
  if (const HWND window = mWindow.exchange(nullptr)) {
    DestroyWindow(window);
  }
  UnregisterClassW(WindowClass, mInstance);
}

void KeybindService::Rebuild() {
  std::vector<Keybinds::BoundAction> bindings;
  const auto add = [&](const Keybinds::Slots& slots, const Keybinds::Action& action) {
    for (const auto& binding: slots) {
      if (binding.IsValid()) {
        bindings.push_back({binding, action});
      }
    }
  };
  for (const auto kind: Keybinds::GlobalActions) {
    add(mPanels.GlobalKeybinds(kind), {kind});
  }
  for (const auto& panel: mPanels.Panels()) {
    for (std::size_t i = 0; i < Keybinds::PanelActions.size(); ++i) {
      add(panel.mKeybinds[i], {Keybinds::PanelActions[i], panel.mID});
    }
  }
  mInput.SetBindings(std::move(bindings));
}

std::optional<Keybinds::Action> KeybindService::UsedBy(const Keybinds::Binding& binding, const Keybinds::Action& except) const {
  const auto check = [&](const Keybinds::Slots& slots, const Keybinds::Action& action) {
    return !(action == except) && std::ranges::find(slots, binding) != slots.end();
  };
  for (const auto kind: Keybinds::GlobalActions) {
    if (const Keybinds::Action global {kind}; check(mPanels.GlobalKeybinds(kind), global)) {
      return global;
    }
  }
  for (const auto& panel: mPanels.Panels()) {
    for (std::size_t i = 0; i < Keybinds::PanelActions.size(); ++i) {
      if (const Keybinds::Action action {Keybinds::PanelActions[i], panel.mID}; check(panel.mKeybinds[i], action)) {
        return action;
      }
    }
  }
  return std::nullopt;
}

std::string KeybindService::Describe(const Keybinds::Action& action) const {
  if (Keybinds::IsGlobal(action.mKind)) {
    return Keybinds::ActionName(action.mKind);
  }
  return std::format("{}: {}", PanelName(mPanels, action.mPanelID), Keybinds::ActionName(action.mKind));
}

void KeybindService::Run(const Keybinds::Action& action) {
  using Keybinds::ActionKind;
  switch (action.mKind) {
    case ActionKind::ToggleAllPanels: {
      const bool showing = mPanels.ToggleAll();
      Log::Info("Keybind: all panels {}", showing ? "shown" : "hidden");
      return;
    }
    case ActionKind::TogglePanel: {
      const bool showing = mPanels.TogglePanel(action.mPanelID);
      Log::Info("Keybind: '{}' {}", PanelName(mPanels, action.mPanelID), showing ? "shown" : "hidden");
      return;
    }
    case ActionKind::BrightnessUp:
    case ActionKind::BrightnessDown: {
      const float step = action.mKind == ActionKind::BrightnessUp ? BrightnessStep : -BrightnessStep;
      if (const auto level = mPanels.ChangeBrightness(action.mPanelID, step)) {
        Log::Info("Keybind: '{}' brightness {:.0f}%", PanelName(mPanels, action.mPanelID), *level * 100);
      }
      return;
    }
    case ActionKind::ToggleTint:
      if (const auto tinted = mPanels.ToggleTint(action.mPanelID)) {
        Log::Info("Keybind: '{}' tint {}", PanelName(mPanels, action.mPanelID), *tinted ? "on" : "off");
      }
      return;
    case ActionKind::ToggleKeyboard:
      if (mToggleKeyboard) {
        Log::Info("Keybind: the keyboard {}", mToggleKeyboard() ? "shown" : "hidden");
      }
      return;
    case ActionKind::Recenter:
      if (mRecenter) {
        Log::Info("Keybind: recenter");
        mRecenter();
      }
      return;
  }
}

LRESULT CALLBACK KeybindService::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
  }
  auto* self = reinterpret_cast<KeybindService*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self && message == FiredMessage && wParam <= static_cast<WPARAM>(Keybinds::LastActionKind)) {
    const Keybinds::Action action {static_cast<Keybinds::ActionKind>(wParam), static_cast<uint64_t>(lParam)};
    ThreadUtil::CatchAll("keybind action", [&] { self->Run(action); });
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}// namespace OpenDesktopVR::Host
