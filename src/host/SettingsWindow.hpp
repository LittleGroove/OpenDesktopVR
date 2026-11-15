// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The desktop settings window (doc 09 "Desktop UI", ADR-020): the panel
// list, a window picker, and each panel's window rule, placement, crop and
// update rate. Dear ImGui on its own small D3D11 device, on the UI thread:
// it can't stall the panel renderer, and the renderer can't stall it.
//
// - Paints only in response to input, and for half a second after (hover
//   and animation), at most 30 times a second; nothing while idle (RN-12).
// - Opens within doc 08's 50 ms: its device is made on a worker thread at
//   start and kept; its window, swapchain and ImGui context exist only
//   while it's open.
// - Window enumeration for the picker runs on a worker thread (ADR-020).
// - Edits are addressed by panel ID and dropped if the panel is gone (DU-2),
//   touch only that panel (ST-6), and save through the debounced store
//   (ST-1). Text fields apply when you leave them, so a window rule isn't
//   re-applied on every keystroke.
#pragma once

#include "PanelManager.hpp"

#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>
#include <OpenDesktopVR/UiTheme.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <Windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ImGuiContext;

namespace OpenDesktopVR::Host {

class SettingsWindow {
 public:
  SettingsWindow(HINSTANCE instance, RenderLoop& renderer, PanelManager& panels);
  ~SettingsWindow();
  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  // Opens the window, or brings it to the front
  void Show();
  bool IsOpen() const noexcept {
    return mWindow != nullptr;
  }
  // The panels changed elsewhere (the tray menu): repaint if open
  void Invalidate();

  // Tests: half a second after opening (and once the picker, if asked for,
  // has its list), save the window as a PNG; then stay idle for two
  // seconds, write how many times it painted in the second of those, and
  // how many input messages it had then, to "<png>.idle.txt" (no paints is
  // a pass: RN-12), and quit the host
  void RequestScreenshot(std::filesystem::path path, bool withPicker);

 private:
  struct Gpu {
    winrt::com_ptr<ID3D11Device> mDevice;
    winrt::com_ptr<ID3D11DeviceContext> mContext;
  };
  // The default adapter, or WARP; empty if neither works
  static Gpu CreateDevice() noexcept;

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
  bool Create();
  void Destroy();
  bool CreateTarget();
  void Resize();
  void Paint();
  void KeepPainting(int frames);
  void SetScale(float scale);
  void TakeScreenshot();
  void OnScreenshotTimer(UINT_PTR timer);

  // The interface, built each paint
  void BuildUi();
  void PanelList(const std::vector<PanelDefinition>& panels);
  void PanelDetails(PanelDefinition panel);
  void WindowPicker();
  void StartListing();
  void OpenPicker(bool retarget);
  void Pick(const WindowUtil::WindowInfo& window);

  // Text fields: loaded when a panel is selected, applied when you leave
  // them, or to the panel they came from if you select another first
  void LoadTextFields(const PanelDefinition& panel);
  void ApplyTextFields(PanelDefinition& panel) const;
  void CommitTextFields();

  HINSTANCE mInstance {nullptr};
  RenderLoop& mRenderer;
  PanelManager& mPanels;

  HWND mWindow {nullptr};
  // Made in the background at start and kept while the host runs (an idle
  // device costs no CPU); the rest exists only while the window is open
  ThreadUtil::BackgroundTask<Gpu> mDeviceTask;
  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext> mContext;
  winrt::com_ptr<IDXGISwapChain1> mSwapchain;
  winrt::com_ptr<ID3D11RenderTargetView> mTarget;
  ImGuiContext* mImGui {nullptr};
  bool mPlatformReady {false};// ImGui's Win32 backend
  bool mRendererReady {false};// ImGui's D3D11 backend
  int mDeviceLosses {0};
  UiTheme::Fonts mFonts;
  float mScale {1.0f};
  bool mRebuildFonts {false};
  bool mOccluded {false};
  int mFramesLeft {0};
  bool mTimerRunning {false};
  uint64_t mPaints {0};

  // What's selected, and the text fields being edited for it
  uint64_t mSelected {0};
  uint64_t mFieldsFor {0};
  std::array<char, 256> mName {};
  std::array<char, 1024> mExecutable {};
  std::array<char, 512> mClass {};
  std::array<char, 512> mTitle {};
  bool mFieldsEdited {false};

  // The window picker
  bool mOpenPicker {false};
  bool mPickerRetargets {false};// "Use a different window" for the selected panel
  ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>> mListing;
  std::vector<WindowUtil::WindowInfo> mWindows;
  std::array<char, 128> mFilter {};
  int mPickerSelection {-1};

  // Tests
  std::optional<std::filesystem::path> mScreenshot;
  bool mScreenshotWithPicker {false};
  bool mScreenshotTaken {false};
  uint64_t mIdleStartPaints {0};
  TimeUtil::TimePoint mOpenedAt {};
  bool mFirstPaintLogged {false};
  // Input and WM_PAINT messages, and the last of them, so the idle check
  // can tell its own painting from a mouse moved over the window
  uint64_t mInputs {0};
  uint64_t mIdleStartInputs {0};
  UINT mLastInput {0};
};

}// namespace OpenDesktopVR::Host
