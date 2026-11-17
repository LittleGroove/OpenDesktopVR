// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "SettingsWindow.hpp"

#include <OpenDesktopVR/Autostart.hpp>
#include <OpenDesktopVR/CrashHandler.hpp>
#include <OpenDesktopVR/ImageFile.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/Theme.hpp>
#include <OpenDesktopVR/UpdatePolicy.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <dwmapi.h>
#include <shellapi.h>
#include <imgui.h>
#include <shobjidl.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <numbers>
#include <utility>

// The Win32 backend's message handler: its header leaves the declaration to
// the app, so the header needn't include <Windows.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace OpenDesktopVR::Host {

namespace {

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.Settings";
constexpr UINT ReopenMessage = WM_APP + 1;// the graphics device was lost
constexpr UINT FileDialogMessage = WM_APP + 2;// a file dialog asked for during a frame

constexpr UINT_PTR PaintTimer = 1;
constexpr UINT_PTR ScreenshotIdleStart = 2;
constexpr UINT_PTR ScreenshotIdleEnd = 3;
constexpr UINT_PTR PreviewTimer = 4;// the live preview: has the panel changed?
constexpr UINT PreviewCheckMs = 100;
constexpr UINT_PTR DiagnosticsTimer = 5;// the Diagnostics tab: has its content changed?
constexpr UINT DiagnosticsCheckMs = 1000;
constexpr float PreviewMaxHeight = 220;
constexpr UINT PaintIntervalMs = 33;// at most about 30 paints a second
constexpr int TailFrames = 15;      // half a second after the last input
constexpr uint64_t ScreenshotAfterPaints = 6;
constexpr int MaxDeviceLosses = 3;

// Sizes at 96 DPI
constexpr int WindowWidth = 960;
constexpr int WindowHeight = 680;
constexpr int MinWidth = 720;
constexpr int MinHeight = 500;
constexpr float ListWidth = 300;
constexpr float PickerThumbnailHeight = 54;// the window picker's pictures
constexpr float PickerProgramColumn = 200;

constexpr auto Clamp = ImGuiSliderFlags_AlwaysClamp;

std::string Utf8(std::wstring_view text) {
  return StringUtil::Narrow(text);
}

template <std::size_t N>
void Fill(std::array<char, N>& buffer, std::wstring_view text) {
  StringUtil::CopyTruncated(StringUtil::Narrow(text), buffer);
}

const char* TitleModeLabel(WindowMatch::TitleMode mode) {
  using enum WindowMatch::TitleMode;
  switch (mode) {
    case Prefer:
      return "Prefer this title";
    case Exact:
      return "Exactly this title";
    case Prefix:
      return "Title starts with this";
    case Contains:
      return "Title contains this";
    case Regex:
      return "Title matches this regular expression";
  }
  return "";
}

constexpr WindowMatch::TitleMode TitleModes[] = {
  WindowMatch::TitleMode::Prefer,
  WindowMatch::TitleMode::Exact,
  WindowMatch::TitleMode::Prefix,
  WindowMatch::TitleMode::Contains,
  WindowMatch::TitleMode::Regex,
};

bool IsInput(UINT message) noexcept {
  return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || (message >= WM_KEYFIRST && message <= WM_KEYLAST)
    || message == WM_MOUSELEAVE || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATE
    || message == WM_INPUTLANGCHANGE;
}

// A dim "(?)" after the last item, explaining it on hover
void Help(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  ImGui::SetItemTooltip("%s", text);
}

// Drag fields: drag left or right to change the number, hold Shift for 4x
// faster or Alt for 5x finer, double-click (or Ctrl+click) to type one.
// Dear ImGui's own modifiers are 10x and 100x; 100x finer was too slow to
// use, so the speed passed in undoes them (test round 5 asked how precise
// placement can be before it gets cumbersome).
float DragSpeed(float speed) {
  const auto& io = ImGui::GetIO();
  if (io.KeyAlt) {
    speed *= 0.2f / 0.01f;
  }
  if (io.KeyShift) {
    speed *= 4.0f / 10.0f;
  }
  return speed;
}

// A length kept in metres, shown and dragged in centimetres
bool DragCentimetres(const char* label, float* metres, float minimumCm, float maximumCm) {
  float cm = *metres * 100.0f;
  if (!ImGui::DragFloat(label, &cm, DragSpeed(0.25f), minimumCm, maximumCm, "%.1f cm", Clamp)) {
    return false;
  }
  *metres = cm / 100.0f;
  return true;
}

bool DragDegrees(const char* label, float* degrees, float minimum, float maximum, ImGuiSliderFlags flags = Clamp) {
  return ImGui::DragFloat(label, degrees, DragSpeed(0.25f), minimum, maximum, "%.1f\xC2\xB0", flags);
}

// A level kept as 0.1 to 1, shown and dragged as 10% to 100%
bool DragPercent(const char* label, float* level) {
  float percent = *level * 100.0f;
  if (!ImGui::DragFloat(label, &percent, DragSpeed(0.5f), 10.0f, 100.0f, "%.0f%%", Clamp)) {
    return false;
  }
  *level = percent / 100.0f;
  return true;
}

bool DragPixels(const char* label, uint32_t* pixels, uint32_t maximum) {
  int value = static_cast<int>(std::min<uint32_t>(*pixels, static_cast<uint32_t>(INT32_MAX)));
  if (!ImGui::DragInt(label, &value, DragSpeed(1.0f), 0, static_cast<int>(maximum), "%d px", Clamp)) {
    return false;
  }
  *pixels = static_cast<uint32_t>(std::max(value, 0));
  return true;
}

}// namespace

SettingsWindow::SettingsWindow(HINSTANCE instance, RenderLoop& renderer, PanelManager& panels, KeybindService& keybinds,
  AppHelper::AppHelperService& appHelper, const Log::LogRing& logRing)
  : mInstance(instance), mRenderer(renderer), mPanels(panels), mKeybinds(keybinds), mAppHelper(appHelper), mLogRing(logRing) {
  mPanelsChanged = mPanels.Changed().Subscribe([this] { Invalidate(); });
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &CrashHandler::GuardedProcedure<&SettingsWindow::WindowProc>;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  // Making a device takes 30 ms or more (180 ms while the driver loads),
  // and fonts about 10 ms: do them now, off this thread, so the window
  // opens quickly
  try {
    mPrepareTask = ThreadUtil::BackgroundTask<Prepared>::Start(L"Settings window", [] { return Prepare(); });
  } catch (...) {
    ThreadUtil::ReportEscapedException("preparing the settings window");
  }
}

SettingsWindow::Prepared SettingsWindow::Prepare() noexcept {
  Prepared prepared;
  prepared.mGpu = CreateDevice();
  try {
    // An atlas needs no ImGui context (ADR-020: the context stays on the UI
    // thread); the main monitor's scale is the usual one
    prepared.mScale = std::clamp(static_cast<float>(GetDpiForSystem()) / USER_DEFAULT_SCREEN_DPI, 0.5f, 4.0f);
    auto atlas = std::make_shared<ImFontAtlas>();
    prepared.mFonts = UiTheme::LoadFonts(*atlas, prepared.mScale);
    if (atlas->Build()) {
      // In the texture's format already, for Dear ImGui's first frame
      unsigned char* pixels = nullptr;
      int width = 0;
      int height = 0;
      atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
      atlas->ClearInputData();// the font files' copies: only needed to build
      prepared.mAtlas = std::move(atlas);
    }
  } catch (...) {
    ThreadUtil::ReportEscapedException("preparing the settings window's fonts");
  }
  return prepared;
}

SettingsWindow::~SettingsWindow() {
  Destroy();
  mContext = nullptr;
  mDevice = nullptr;
  UnregisterClassW(WindowClass, mInstance);
}

SettingsWindow::Gpu SettingsWindow::CreateDevice() noexcept {
  Gpu gpu;
  constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  HRESULT result = E_FAIL;
  for (const auto driver: {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    result = D3D11CreateDevice(nullptr, driver, nullptr, 0, levels, static_cast<UINT>(std::size(levels)),
      D3D11_SDK_VERSION, gpu.mDevice.put(), nullptr, gpu.mContext.put());
    if (SUCCEEDED(result)) {
      return gpu;
    }
  }
  Log::Warning("Settings window: no Direct3D 11 device ({:#010x})", static_cast<uint32_t>(result));
  return {};
}

void SettingsWindow::Show() {
  if (mWindow) {
    if (IsIconic(mWindow)) {
      ShowWindow(mWindow, SW_RESTORE);
    }
    SetForegroundWindow(mWindow);
    return;
  }
  mOpenedAt = TimeUtil::Now();
  mOpeningMark = mOpenedAt;
  mOpeningParts.clear();
  mFirstPaintLogged = false;
  // Other programs may have changed the file since it was loaded
  mPanels.Refresh();
  MarkOpening("settings file");
  bool created = false;
  ThreadUtil::CatchAll("opening the settings window", [&] { created = Create(); });
  if (!created) {
    Destroy();
    Log::Warning("Settings window: couldn't be opened");
    return;
  }
  Log::Info("Settings window opened ({:.0f}% scale)", mScale * 100);
}

void SettingsWindow::MarkOpening(const char* part) {
  if (mFirstPaintLogged) {
    return;
  }
  const auto now = TimeUtil::Now();
  mOpeningParts += std::format("{}{} {:.0f}", mOpeningParts.empty() ? "" : ", ", part,
    TimeUtil::ToMilliseconds(now - mOpeningMark));
  mOpeningMark = now;
}

void SettingsWindow::Invalidate() {
  if (mWindow) {
    KeepPainting(2);
  }
}

void SettingsWindow::RequestScreenshot(std::filesystem::path path, bool withPicker) {
  mScreenshot = std::move(path);
  mScreenshotWithPicker = withPicker;
  mScreenshotTaken = false;
  if (withPicker) {
    OpenPicker(false);
  }
}

bool SettingsWindow::Create() {
  // On the monitor the user is working on (the tray icon they clicked)
  POINT cursor {};
  GetCursorPos(&cursor);
  MONITORINFO monitor {sizeof(monitor)};
  GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
  const RECT work = monitor.rcWork;

  const auto title = std::format(L"{} settings", Product::DisplayName);
  mWindow = CreateWindowExW(0, WindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, work.left + 32, work.top + 32, 640, 480,
    nullptr, nullptr, mInstance, this);
  if (!mWindow) {
    return false;
  }
  // Sized for that monitor's DPI, centred on it
  SetScale(static_cast<float>(GetDpiForWindow(mWindow)) / USER_DEFAULT_SCREEN_DPI);
  const int workWidth = work.right - work.left;
  const int workHeight = work.bottom - work.top;
  const int width = std::min(static_cast<int>(std::lround(WindowWidth * mScale)), workWidth);
  const int height = std::min(static_cast<int>(std::lround(WindowHeight * mScale)), workHeight);
  SetWindowPos(mWindow, nullptr, work.left + (workWidth - width) / 2, work.top + (workHeight - height) / 2, width, height,
    SWP_NOZORDER | SWP_NOACTIVATE);
  // A dark title bar to match (Windows 10 20H1 and later; older ones ignore it)
  constexpr DWORD UseImmersiveDarkMode = 20;
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(mWindow, UseImmersiveDarkMode, &dark, sizeof(dark));
  MarkOpening("window");

  // Its own device: nothing here can wait on the panel renderer's GPU work.
  // Usually made in the background at start (and kept), so opening the
  // window stays within doc 08's 50 ms.
  if (mDevice && mDevice->GetDeviceRemovedReason() != S_OK) {
    mDevice = nullptr;
    mContext = nullptr;
  }
  if (mPrepareTask.Started() && !mPrepareTask.Running()) {
    if (auto prepared = mPrepareTask.Take()) {
      if (!mDevice && prepared->mGpu.mDevice) {
        mDevice = std::move(prepared->mGpu.mDevice);
        mContext = std::move(prepared->mGpu.mContext);
      }
      if (!mAtlas && prepared->mAtlas) {
        mAtlas = std::move(prepared->mAtlas);
        mAtlasScale = prepared->mScale;
        mFonts = prepared->mFonts;
      }
    }
  }
  if (!mDevice) {
    if (!mDevice) {
      auto gpu = CreateDevice();// not ready (or failed): never wait for it
      mDevice = std::move(gpu.mDevice);
      mContext = std::move(gpu.mContext);
    }
  }
  if (!mDevice) {
    return false;
  }
  MarkOpening("device");
  HRESULT result = S_OK;
  winrt::com_ptr<IDXGIAdapter> adapter;
  winrt::com_ptr<IDXGIFactory2> factory;
  winrt::check_hresult(mDevice.as<IDXGIDevice>()->GetAdapter(adapter.put()));
  winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));
  DXGI_SWAP_CHAIN_DESC1 desc {};
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc = {1, 0};
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  result = factory->CreateSwapChainForHwnd(mDevice.get(), mWindow, &desc, nullptr, nullptr, mSwapchain.put());
  if (FAILED(result)) {
    Log::Warning("Settings window: no swapchain ({:#010x})", static_cast<uint32_t>(result));
    return false;
  }
  factory->MakeWindowAssociation(mWindow, DXGI_MWA_NO_ALT_ENTER);
  if (!CreateTarget()) {
    return false;
  }
  MarkOpening("swapchain");

  // One ImGui context, for this thread only (ADR-020), with our atlas
  if (!mAtlas) {
    mAtlas = std::make_shared<ImFontAtlas>();// built on the first frame
    mAtlasScale = 0;
  }
  mImGui = ImGui::CreateContext(mAtlas.get());
  ImGui::SetCurrentContext(mImGui);
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;// nothing written next to the exe
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigInputTextCursorBlink = false;// a blinking caret would mean painting forever
  mPlatformReady = ImGui_ImplWin32_Init(mWindow);
  mRendererReady = mPlatformReady && ImGui_ImplDX11_Init(mDevice.get(), mContext.get());
  if (!mRendererReady) {
    Log::Warning("Settings window: Dear ImGui didn't start");
    return false;
  }
  MarkOpening("Dear ImGui");

  // The first frame before the window shows, so it appears with it.
  // Showing it waits for Windows to set up text input for this thread
  // (about 10 ms, the first time), and WM_PAINT comes later still.
  Paint();
  ShowWindow(mWindow, SW_SHOWNORMAL);
  SetForegroundWindow(mWindow);
  KeepPainting(TailFrames);
  return true;
}

void SettingsWindow::Destroy() {
  mDiagnosticsTimer = false;// timers go with the window
  mDiagnosticsDrawn = false;
  // The preview's views are on this window's device; the link goes with it
  mPreviewViews.clear();
  mLink.reset();
  mPreviewPanel = 0;
  mThumbnails.clear();
  if (mImGui) {
    ImGui::SetCurrentContext(mImGui);
    if (mRendererReady) {
      ImGui_ImplDX11_Shutdown();
    }
    if (mPlatformReady) {
      ImGui_ImplWin32_Shutdown();
    }
    ImGui::DestroyContext(mImGui);
    mImGui = nullptr;
  }
  mRendererReady = false;
  mPlatformReady = false;
  // mFonts stay: they live in mAtlas, which is kept for next time
  mTarget = nullptr;
  mSwapchain = nullptr;// before its window goes
  if (mContext) {
    // The device stays for next time; everything bound to it goes
    mContext->ClearState();
    mContext->Flush();
  }
  mFramesLeft = 0;
  mTimerRunning = false;
  mOccluded = false;
  mFieldsFor = 0;
  mFieldsEdited = false;
  mListing = {};// never waits for the worker
  if (mCapture) {
    mCapture->Cancel();// the session outlives the dialog (KY-4); it just ends
    mCapture.reset();
  }
  mWindows.clear();
  if (mWindow) {
    // Messages from here on go to DefWindowProc
    DestroyWindow(std::exchange(mWindow, nullptr));
  }
}

bool SettingsWindow::CreateTarget() {
  winrt::com_ptr<ID3D11Texture2D> back;
  if (FAILED(mSwapchain->GetBuffer(0, IID_PPV_ARGS(back.put())))) {
    return false;
  }
  return SUCCEEDED(mDevice->CreateRenderTargetView(back.get(), nullptr, mTarget.put()));
}

void SettingsWindow::Resize() {
  if (!mSwapchain) {
    return;
  }
  mTarget = nullptr;
  mContext->OMSetRenderTargets(0, nullptr, nullptr);
  // 0 x 0: the client area's size
  if (const HRESULT result = mSwapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0); FAILED(result)) {
    Log::Warning("Settings window: couldn't resize ({:#010x})", static_cast<uint32_t>(result));
    return;
  }
  CreateTarget();
}

void SettingsWindow::SetScale(float scale) {
  mScale = std::clamp(scale, 0.5f, 4.0f);
  mRebuildFonts = true;
}

void SettingsWindow::KeepPainting(int frames) {
  if (!mWindow) {
    return;
  }
  mFramesLeft = std::max(mFramesLeft, frames);
  if (!mTimerRunning) {
    // Paint the first frame now (when the queue is empty), then on the timer
    InvalidateRect(mWindow, nullptr, FALSE);
    mTimerRunning = SetTimer(mWindow, PaintTimer, PaintIntervalMs, nullptr) != 0;
  }
}

LRESULT CALLBACK SettingsWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self && self->mWindow == window) {
    LRESULT result = 0;
    if (ThreadUtil::CatchAll("settings window", [&] { result = self->Handle(message, wParam, lParam); })) {
      return result;
    }
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT SettingsWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == FileDialogMessage) {
    RunPendingDialog();
    return 0;
  }
  if (mImGui) {
    ImGui::SetCurrentContext(mImGui);
    if (ImGui_ImplWin32_WndProcHandler(mWindow, message, wParam, lParam)) {
      KeepPainting(TailFrames);
      return TRUE;
    }
  }
  if (IsInput(message)) {
    ++mInputs;
    mLastInput = message;
    KeepPainting(TailFrames);
  }
  switch (message) {
    case WM_PAINT: {
      ++mInputs;
      mLastInput = message;
      PAINTSTRUCT paint {};
      BeginPaint(mWindow, &paint);
      EndPaint(mWindow, &paint);
      Paint();
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;// Paint covers everything
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) {
        Resize();
        Paint();// now, so resizing looks smooth
      }
      return 0;
    case WM_TIMER:
      if (wParam == PreviewTimer) {
        CheckPreview();
        return 0;
      }
      if (wParam == DiagnosticsTimer) {
        CheckDiagnostics();
        return 0;
      }
      if (wParam == PaintTimer) {
        Paint();
        if (mWindow && --mFramesLeft <= 0 && !mListing.Started()) {
          KillTimer(mWindow, PaintTimer);
          mTimerRunning = false;
        }
      } else {
        OnScreenshotTimer(wParam);
      }
      return 0;
    case WM_DPICHANGED: {
      SetScale(static_cast<float>(HIWORD(wParam)) / USER_DEFAULT_SCREEN_DPI);
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      SetWindowPos(mWindow, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
        suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      KeepPainting(TailFrames);
      return 0;
    }
    case WM_SYSCOMMAND:
      // Alt on its own (the fine-drag key) would open the window menu when
      // released. Alt+Space and Alt+F4 still work.
      if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) {
        return 0;
      }
      break;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
      info->ptMinTrackSize = {std::lround(MinWidth * mScale), std::lround(MinHeight * mScale)};
      return 0;
    }
    case WM_CLOSE:
      CommitTextFields();
      Destroy();
      Log::Info("Settings window closed");
      return 0;
    case ReopenMessage:
      Destroy();
      Show();
      return 0;
  }
  return DefWindowProcW(mWindow, message, wParam, lParam);
}

void SettingsWindow::Paint() {
  if (!mWindow || !mImGui || !mSwapchain || mInFrame) {
    return;
  }
  mInFrame = true;
  struct FrameDone {
    bool& mInFrame;
    ~FrameDone() {
      mInFrame = false;
    }
  } frameDone {mInFrame};
  const auto frameStart = TimeUtil::Now();
  const bool switching = mCycleTab >= 0;
  if (mOccluded) {
    if (mSwapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
      return;// minimized or covered: nothing to paint
    }
    mOccluded = false;
  }
  if (!mTarget && !CreateTarget()) {
    return;
  }
  ImGui::SetCurrentContext(mImGui);
  MarkOpening("waiting to paint");
  if (mRebuildFonts) {
    UiTheme::Apply(mScale);
    auto& io = ImGui::GetIO();
    if (mAtlasScale != mScale) {
      // Another scale than the atlas has (another monitor): rebuilt here
      mFonts = UiTheme::LoadFonts(*io.Fonts, mScale);
      mAtlasScale = mScale;
      ImGui_ImplDX11_InvalidateDeviceObjects();// the next frame rebuilds the font texture
      MarkOpening("fonts");
    }
    io.FontDefault = mFonts.mBody;
    mRebuildFonts = false;
  }
  if (!ImGui::GetIO().Fonts->IsBuilt()) {
    ImGui::GetIO().Fonts->Build();// done by the next call anyway; here it's timed
    MarkOpening("font atlas");
  }
  ImGui_ImplDX11_NewFrame();
  MarkOpening("shaders and font texture");
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  BuildUi();
  ImGui::Render();

  const auto background = Theme::ToRgba(Theme::Background);
  const float clear[4] {background.r, background.g, background.b, 1.0f};
  auto* target = mTarget.get();
  mContext->OMSetRenderTargets(1, &target, nullptr);
  mContext->ClearRenderTargetView(target, clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  ++mPaints;

  if (mScreenshot && !mScreenshotTaken) {
    // Half a second: popups' fade-ins are done
    const bool settled = TimeUtil::Now() - mOpenedAt >= std::chrono::milliseconds(500);
    // A preview on the page: its first image (the host starts drawing once
    // this window watches), or 3 s
    const bool previewReady = !mPreviewDrawn || mPreviewShowing || TimeUtil::Now() - mOpenedAt >= std::chrono::seconds(3);
    if (settled && previewReady && mPaints >= ScreenshotAfterPaints && !mListing.Started() && !mThumbnailTask.Started()
        && !mSelfTestTask.Started()) {
      TakeScreenshot();// before Present: a flip-model buffer is undefined after it
    } else {
      KeepPainting(2);
    }
  }

  MarkOpening("drawing");
  const HRESULT result = mSwapchain->Present(1, 0);
  if (switching && mFirstPaintLogged) {
    // DU-3: a page switch is one frame
    mLongestSwitchMs = std::max(mLongestSwitchMs, TimeUtil::ToMilliseconds(TimeUtil::Now() - frameStart));
  }
  if (!mFirstPaintLogged) {
    // Doc 08: opening the window takes at most 50 ms
    MarkOpening("present");
    mFirstPaintLogged = true;
    mOpeningMs = std::chrono::duration_cast<std::chrono::milliseconds>(TimeUtil::Now() - mOpenedAt).count();
    Log::Info("Settings window: first frame {} ms after opening ({})", mOpeningMs, mOpeningParts);
  }
  if (result == DXGI_STATUS_OCCLUDED) {
    mOccluded = true;
  } else if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
    const auto reason = mDevice->GetDeviceRemovedReason();
    if (++mDeviceLosses <= MaxDeviceLosses) {
      Log::Warning("Settings window: the graphics device was lost ({:#010x}); reopening", static_cast<uint32_t>(reason));
      PostMessageW(mWindow, ReopenMessage, 0, 0);
    } else {
      Log::Warning("Settings window: the graphics device was lost again ({:#010x}); closing", static_cast<uint32_t>(reason));
      PostMessageW(mWindow, WM_CLOSE, 0, 0);
    }
  }
}

void SettingsWindow::TakeScreenshot() {
  mScreenshotTaken = true;
  winrt::com_ptr<ID3D11Texture2D> back;
  bool saved = false;
  if (SUCCEEDED(mSwapchain->GetBuffer(0, IID_PPV_ARGS(back.put())))) {
    if (const auto image = ImageFile::ReadTexture(mDevice.get(), mContext.get(), back.get())) {
      saved = ImageFile::SavePng(*mScreenshot, *image);
    }
  }
  Log::Info("Settings window: screenshot {}: {}", saved ? "saved" : "FAILED", StringUtil::Narrow(mScreenshot->wstring()));
  SetTimer(mWindow, ScreenshotIdleStart, 1000, nullptr);
}

void SettingsWindow::OnScreenshotTimer(UINT_PTR timer) {
  if (timer == ScreenshotIdleStart) {
    // A second after the screenshot, any painting has stopped: count paints
    // over the next second with no input
    KillTimer(mWindow, ScreenshotIdleStart);
    mIdleStartPaints = mPaints;
    mIdleStartInputs = mInputs;
    SetTimer(mWindow, ScreenshotIdleEnd, 1000, nullptr);
  } else if (timer == ScreenshotIdleEnd) {
    KillTimer(mWindow, ScreenshotIdleEnd);
    const auto idlePaints = mPaints - mIdleStartPaints;
    const auto inputs = mInputs - mIdleStartInputs;
    Log::Info(
      "Settings window: {} paints and {} input messages (last {:#x}) in the idle second", idlePaints, inputs, mLastInput);
    auto report = *mScreenshot;
    report += L".idle.txt";
    std::ofstream {report} << std::format(
      "idle paints: {}\ninput messages: {} (last {:#x})\nopened in: {} ms\nlongest page switch: {:.1f} ms\n", idlePaints,
      inputs, mLastInput, mOpeningMs, mLongestSwitchMs);
    PostQuitMessage(0);
  }
}

// ---- The interface ---------------------------------------------------------

void SettingsWindow::BuildUi() {
  // The picker's list, once the worker has it; then its pictures
  if (mListing.Started() && !mListing.Running()) {
    mWindows = mListing.Take().value_or(std::vector<WindowUtil::WindowInfo> {});
    mPickerSelection = -1;
    KeepPainting(TailFrames);
    StartThumbnails();
  }
  TakeThumbnails();

  mPreviewDrawn = false;
  mPreviewShowing = false;
  mDiagnosticsWasDrawn = std::exchange(mDiagnosticsDrawn, false);
  TakeSelfTest();
  const auto panels = mPanels.Panels();// a copy: edits below replace the originals
  const auto selected = std::ranges::find(panels, mSelected, &PanelDefinition::mID);
  if (selected == panels.end()) {
    // Nothing selected yet, or the panel is gone (DU-2)
    mSelected = panels.empty() ? 0 : panels.front().mID;
  }

  const auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
    | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("Settings", nullptr, flags)) {
    // Heading, and whether panels show at all
    ImGui::PushFont(mFonts.mHeading);
    ImGui::TextUnformatted(Utf8(Product::DisplayName).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Utf8(Product::Tagline).c_str());
    const char* visibleLabel = "Show panels in VR";
    const float visibleWidth
      = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(visibleLabel).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - visibleWidth));
    // Show or hide all (PN-6): ticked while any panel is showing
    bool showing = mPanels.AnyShowing();
    if (ImGui::Checkbox(visibleLabel, &showing)) {
      Log::Info("Panels {} (settings window)", mPanels.ToggleAll() ? "shown" : "hidden");
    }
    ImGui::SetItemTooltip("Shows or hides all panels, like the keybind for it");

    if (ImGui::BeginTabBar("pages", ImGuiTabBarFlags_DrawSelectedOverline)) {
      // Tests (DU-3): each tab in turn, one a frame
      constexpr int TabCount = 6;
      const auto cycle = [&](int index) {
        return mCycleTab == index ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
      };
      if (mCycleTab >= 0) {
        KeepPainting(2);
      }
      if (ImGui::BeginTabItem("Panels", nullptr, cycle(0))) {
        PanelList(panels);
        ImGui::SameLine();
        const auto current = std::ranges::find(panels, mSelected, &PanelDefinition::mID);
        if (current != panels.end()) {
          PanelDetails(*current);
        } else {
          ImGui::BeginChild("details");
          ImGui::TextWrapped("No panels yet. Choose \"Add a window...\" to see one of your windows in VR.");
          ImGui::EndChild();
        }
        ImGui::EndTabItem();
      }
      const auto keybindsFlags = (std::exchange(mSelectKeybinds, false) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None) | cycle(1);
      if (ImGui::BeginTabItem("Keybinds", nullptr, keybindsFlags)) {
        KeybindsPage(panels);
        ImGui::EndTabItem();
      }
      const auto handsFlags = (std::exchange(mSelectHands, false) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None) | cycle(2);
      if (ImGui::BeginTabItem("Hands", nullptr, handsFlags)) {
        HandsPage();
        ImGui::EndTabItem();
      }
      const auto profilesFlags = (std::exchange(mSelectProfiles, false) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None) | cycle(3);
      if (ImGui::BeginTabItem("Profiles", nullptr, profilesFlags)) {
        ProfilesPage();
        ImGui::EndTabItem();
      }
      const auto generalFlags = (std::exchange(mSelectGeneral, false) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None) | cycle(4);
      if (ImGui::BeginTabItem("General", nullptr, generalFlags)) {
        GeneralPage();
        ImGui::EndTabItem();
      }
      const auto diagnosticsFlags = (std::exchange(mSelectDiagnostics, false) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None) | cycle(5);
      if (ImGui::BeginTabItem("Diagnostics", nullptr, diagnosticsFlags)) {
        DiagnosticsPage();
        ImGui::EndTabItem();
      }
      if (mCycleTab >= 0 && ++mCycleTab > TabCount) {
        mCycleTab = -1;// done: back to the first
      }
      ImGui::EndTabBar();
    }
    WindowPicker();
    CapturePopup();
    PrivacyPopup();
  }
  ImGui::End();
}

void SettingsWindow::PanelList(const std::vector<PanelDefinition>& panels) {
  const float width = ListWidth * mScale;
  ImGui::BeginGroup();
  if (ImGui::BeginChild("list", {width, -ImGui::GetFrameHeightWithSpacing()}, ImGuiChildFlags_Borders)) {
    // Each row: the shown-in-VR toggle, then the name over what it shows
    // (doc 09 "Desktop UI")
    const float line = ImGui::GetTextLineHeight();
    const float rowHeight = std::max(line * 2 + ImGui::GetStyle().FramePadding.y, ImGui::GetFrameHeight());
    for (const auto& panel: panels) {
      ImGui::PushID(reinterpret_cast<const void*>(static_cast<uintptr_t>(panel.mID)));
      const float top = ImGui::GetCursorPosY();
      ImGui::SetCursorPosY(top + (rowHeight - ImGui::GetFrameHeight()) / 2);
      bool enabled = panel.mEnabled;
      if (ImGui::Checkbox("##shown", &enabled)) {
        auto edited = panel;
        edited.mEnabled = enabled;
        mPanels.Update(edited);
      }
      ImGui::SetItemTooltip("%s", enabled ? "Shown in VR. Click to hide it." : "Hidden. Click to show it in VR.");
      ImGui::SameLine();
      ImGui::SetCursorPosY(top);
      const ImVec2 corner = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##panel", panel.mID == mSelected, ImGuiSelectableFlags_None, {0, rowHeight})) {
        mSelected = panel.mID;
      }
      const auto name = Utf8(panel.mName.empty() ? L"(no name)" : panel.mName);
      const auto program = StringUtil::FileNamePart(panel.mWindow.mExecutablePath);
      const auto showing = panel.mKind == PanelKind::TestPattern ? std::string {"Test pattern"}
        : !program.empty()                                       ? Utf8(program)
        : !panel.mWindow.mTitle.empty()                          ? Utf8(panel.mWindow.mTitle)
                                                                 : std::string {"Any window"};
      auto* draw = ImGui::GetWindowDrawList();
      const float inset = ImGui::GetStyle().FramePadding.x / 2;
      const float textTop = corner.y + (rowHeight - line * 2) / 2;
      draw->PushClipRect(corner, ImGui::GetItemRectMax(), true);
      draw->AddText({corner.x + inset, textTop}, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
      const auto second = panel.mEnabled && mPanels.IsHidden(panel.mID) ? showing + "  \xC2\xB7  hidden for now" : showing;
      draw->AddText({corner.x + inset, textTop + line}, ImGui::GetColorU32(ImGuiCol_TextDisabled), second.c_str());
      draw->PopClipRect();
      ImGui::PopID();
    }
    if (panels.empty()) {
      ImGui::TextDisabled("No panels yet");
    }
  }
  ImGui::EndChild();
  if (ImGui::Button("Add a window...", {width, 0})) {
    OpenPicker(false);
  }
  ImGui::EndGroup();
}

void SettingsWindow::PanelDetails(PanelDefinition panel) {
  if (!ImGui::BeginChild("details")) {
    ImGui::EndChild();
    return;
  }
  if (mFieldsFor != panel.mID) {
    CommitTextFields();// to the panel they came from
    LoadTextFields(panel);
  }
  bool changed = false;
  bool removed = false;
  const bool isWindow = panel.mKind == PanelKind::Window;

  ImGui::PushFont(mFonts.mHeading);
  ImGui::TextUnformatted(Utf8(panel.mName.empty() ? L"(no name)" : panel.mName).c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s", isWindow ? "Shows a window from your desktop" : "A test pattern");
  ImGui::Spacing();
  PanelPreview(panel);
  ImGui::Spacing();
  ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);

  // Text fields apply when you leave them
  const auto textField = [&](const char* label, auto& buffer) {
    if (ImGui::InputText(label, buffer.data(), buffer.size())) {
      mFieldsEdited = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      ApplyTextFields(panel);
      mFieldsEdited = false;
      changed = true;
    }
  };
  textField("Name", mName);

  if (isWindow) {
    ImGui::SeparatorText("Which window");
    textField("Program", mExecutable);
    Help("The program's full path. Empty matches any program.");
    textField("Window class", mClass);
    Help("Set by the program, and the same every time it runs. Empty matches any class.");
    textField("Title", mTitle);
    if (ImGui::BeginCombo("Title rule", TitleModeLabel(panel.mWindow.mTitleMode))) {
      for (const auto mode: TitleModes) {
        if (ImGui::Selectable(TitleModeLabel(mode), mode == panel.mWindow.mTitleMode)) {
          ApplyTextFields(panel);// keep unsaved text
          mFieldsEdited = false;
          panel.mWindow.mTitleMode = mode;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    Help(
      "\"Prefer this title\" finds the program's window even when its title changes (browser tabs, songs, "
      "channels), and uses the title only to choose between several of its windows.");
    if (ImGui::Button("Use a different window...")) {
      OpenPicker(true);
    }
  } else {
    ImGui::SeparatorText("Pattern");
    changed |= ImGui::Checkbox("Animated", &panel.mAnimated);
  }

  ImGui::SeparatorText("Where it is");
  ImGui::TextDisabled("From where your head was when you last recentered.");
  ImGui::TextDisabled("Drag a number left or right. Shift: faster. Alt: finer. Double-click: type.");
  changed |= DragCentimetres("Left / right", &panel.mPosition[0], -500.0f, 500.0f);
  changed |= DragCentimetres("Down / up", &panel.mPosition[1], -300.0f, 300.0f);
  float distance = -panel.mPosition[2];
  if (DragCentimetres("Distance", &distance, 10.0f, 1000.0f)) {
    panel.mPosition[2] = -distance;
    changed = true;
  }
  changed |= DragCentimetres("Width", &panel.mWidthMeters, 5.0f, 500.0f);
  Help("Its height follows the window's shape.");
  // PN-14: a box, the content keeping its shape inside it
  bool limitHeight = panel.mMaxHeightMeters > 0;
  if (ImGui::Checkbox("Limit its height", &limitHeight)) {
    panel.mMaxHeightMeters = limitHeight ? std::max(0.05f, panel.mWidthMeters * 0.75f) : 0.0f;
    changed = true;
  }
  Help("For a window that might get tall: the panel shrinks to fit this height, keeping its shape.");
  if (limitHeight) {
    changed |= DragCentimetres("Height at most", &panel.mMaxHeightMeters, 5.0f, 500.0f);
  }
  bool faceMe = panel.mFaceOrigin;
  if (ImGui::Checkbox("Face me", &faceMe)) {
    if (!faceMe) {
      // Start from the way it faces now, so it doesn't jump
      const auto facing = FacingOrigin(panel.mPosition);
      panel.mYawDegrees = facing.mYawDegrees;
      panel.mPitchDegrees = facing.mPitchDegrees;
    }
    panel.mFaceOrigin = faceMe;
    changed = true;
  }
  Help("Turns and tilts the panel toward your head wherever you put it. Untick it to set the angles yourself.");
  if (!panel.mFaceOrigin) {
    changed |= DragDegrees("Turn", &panel.mYawDegrees, -180.0f, 180.0f, Clamp | ImGuiSliderFlags_WrapAround);
    Help("Positive turns it to its left.");
    changed |= DragDegrees("Tilt", &panel.mPitchDegrees, -90.0f, 90.0f);
    Help("Positive tilts its top away from you, so it faces up: for a panel below your eyes.");
  }

  if (isWindow) {
    ImGui::SeparatorText("Trim the edges");
    // In pixels of the window, so a trim stays on the title bar when the
    // window is resized; each edge can go up to what the others leave
    const auto window = mRenderer.UntrimmedSize(panel.mID);
    const uint32_t width = window ? window->mWidth : 4000;
    const uint32_t height = window ? window->mHeight : 4000;
    const auto upTo = [](uint32_t size, uint32_t other) {
      const uint32_t keep = std::min(size, Render::Crop::MinimumLeft);
      return size > other + keep ? size - other - keep : 0u;
    };
    auto& crop = panel.mCrop;
    changed |= DragPixels("Top edge", &crop.mTop, upTo(height, crop.mBottom));
    Help("For title bars, tabs and toolbars the program draws inside its window. Windows' own frame is already trimmed.");
    changed |= DragPixels("Bottom edge", &crop.mBottom, upTo(height, crop.mTop));
    changed |= DragPixels("Left edge", &crop.mLeft, upTo(width, crop.mRight));
    changed |= DragPixels("Right edge", &crop.mRight, upTo(width, crop.mLeft));
    if (window) {
      const auto kept = crop.Apply(*window);
      ImGui::TextDisabled("The window is %u x %u pixels; %u x %u are shown.", window->mWidth, window->mHeight,
        kept.mWidth, kept.mHeight);
    } else {
      ImGui::TextDisabled("The window's size shows here once the window is open.");
    }
    ImGui::BeginDisabled(crop.IsEmpty());
    if (ImGui::Button("Don't trim")) {
      crop = {};
      changed = true;
    }
    ImGui::EndDisabled();
  }

  ImGui::SeparatorText("How it looks");
  changed |= DragPercent("Opacity", &panel.mOpacity);
  changed |= DragPercent("When you look at it", &panel.mFocusedOpacity);
  Help("Its opacity while your head points at it. Set Opacity lower and this higher to see through it until you look.");
  changed |= DragPercent("Brightness", &panel.mBrightness);
  changed |= ImGui::Checkbox("Tint", &panel.mTintEnabled);
  ImGui::SameLine();
  float tint[3] {static_cast<float>((panel.mTintColor >> 16) & 0xFF) / 255.0f,
    static_cast<float>((panel.mTintColor >> 8) & 0xFF) / 255.0f, static_cast<float>(panel.mTintColor & 0xFF) / 255.0f};
  if (ImGui::ColorEdit3("Tint colour", tint, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
    const auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    panel.mTintColor = (byte(tint[0]) << 16) | (byte(tint[1]) << 8) | byte(tint[2]);
    changed = true;
  }
  Help("A colour over the panel, for night flying. Click the colour to change it.");
  ImGui::TextDisabled("Keybinds for this panel (show or hide, brighter, dimmer, tint) are on the Keybinds tab.");

  // Doc 05, doc 09 "Interaction"
  ImGui::SeparatorText("Hands");
  changed |= ImGui::Checkbox("Tap and drag with your finger", &panel.mTouch);
  Help("Point with your index finger and touch the panel to click. Hold your finger on it to drag.");
  changed |= ImGui::Checkbox("Scroll with two fingers", &panel.mScroll);
  Help("Touch with your index and middle fingers out, then move them up or down.");
  if (isWindow && (panel.mTouch || panel.mScroll)) {
    // The helper's input mode, ready from capture start (HK-1)
    if (const auto status = mAppHelper.InputStatusOf(panel.mID)) {
      ImGui::TextDisabled("%s", status->mText.c_str());
      if (status->mState == AppHelper::AppHelperService::State::Waiting) {
        KeepPainting(15);// until the helper answers
      }
    } else {
      ImGui::TextDisabled("%s", mPanels.Hands().mEnabled ? "Shows here once the window is captured."
                                                         : "Hands are off (Hands tab).");
    }
  }

  ImGui::SeparatorText("Updates");
  int rate = static_cast<int>(std::lround(panel.mMaxUpdatesPerSecond));
  const int maxRate = static_cast<int>(std::lround(mPanels.MaxUpdatesPerSecond()));
  const auto fastest = std::format("Fastest ({} a second)", maxRate);
  if (ImGui::SliderInt("Most updates a second", &rate, 0, std::max(maxRate, rate), rate == 0 ? fastest.c_str() : "%d a second",
        Clamp)) {
    panel.mMaxUpdatesPerSecond = rate;
    changed = true;
  }
  Help("Lower is lighter on your PC. Panels only update when what they show changes.");
  if (rate > maxRate) {
    // PN-12: shown clamped, not silently changed
    ImGui::TextDisabled("Capped at %d a second by the General tab's Performance setting.", maxRate);
  }
  if (isWindow) {
    // CT-3, Q10 (ADR-031): on by default, with what the helper is doing
    changed |= ImGui::Checkbox("Keep updating when covered", &panel.mKeepUpdatingWhenCovered);
    Help(
      "Browsers (Firefox, Edge, Chrome) and apps built like them (Discord, Spotify) stop drawing when another "
      "window, like the game, covers them. This loads a small helper into that app so it keeps drawing. Untick it "
      "if the app misbehaves.");
    if (const auto status = mAppHelper.StatusOf(panel.mID)) {
      ImGui::TextDisabled("%s", status->mText.c_str());
      if (status->mState == AppHelper::AppHelperService::State::Waiting) {
        KeepPainting(15);// until the helper answers
      }
    } else {
      ImGui::TextDisabled("%s", panel.mKeepUpdatingWhenCovered ? "Shows here once the window is captured." : "Off.");
    }
  }
  ImGui::PopItemWidth();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (ImGui::Button("Remove this panel")) {
    ImGui::OpenPopup("Remove panel");
  }
  if (ImGui::BeginPopupModal("Remove panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::Text("Remove \"%s\"?", Utf8(panel.mName).c_str());
    ImGui::TextDisabled("Its placement and settings are deleted.");
    ImGui::Spacing();
    if (ImGui::Button("Remove")) {
      mPanels.Remove(panel.mID, false);
      mFieldsFor = 0;
      mFieldsEdited = false;
      removed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (changed && !removed) {
    mPanels.Update(panel);
  }
  ImGui::EndChild();
}

void SettingsWindow::LoadTextFields(const PanelDefinition& panel) {
  Fill(mName, panel.mName);
  Fill(mExecutable, panel.mWindow.mExecutablePath);
  Fill(mClass, panel.mWindow.mClassName);
  Fill(mTitle, panel.mWindow.mTitle);
  mFieldsFor = panel.mID;
  mFieldsEdited = false;
}

void SettingsWindow::ApplyTextFields(PanelDefinition& panel) const {
  panel.mName = StringUtil::Widen(mName.data());
  if (panel.mKind == PanelKind::Window) {
    panel.mWindow.mExecutablePath = StringUtil::Widen(mExecutable.data());
    panel.mWindow.mClassName = StringUtil::Widen(mClass.data());
    panel.mWindow.mTitle = StringUtil::Widen(mTitle.data());
  }
}

void SettingsWindow::CommitTextFields() {
  if (!mFieldsEdited || mFieldsFor == 0) {
    return;
  }
  mFieldsEdited = false;
  if (auto panel = mPanels.Find(mFieldsFor)) {// DU-2: it may be gone
    ApplyTextFields(*panel);
    mPanels.Update(*panel);
  }
}

// ---- The window picker -----------------------------------------------------

void SettingsWindow::StartListing() {
  if (mListing.Started()) {
    return;
  }
  try {
    mListing = ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>>::Start(
      L"Window list", [] { return WindowUtil::ListCaptureCandidates(); });
  } catch (...) {
    ThreadUtil::ReportEscapedException("listing windows");
  }
  KeepPainting(TailFrames);// polls until the list arrives
}

void SettingsWindow::OpenPicker(bool retarget) {
  mPickerRetargets = retarget;
  mOpenPicker = true;
  KeepPainting(TailFrames);
}

void SettingsWindow::PanelPreview(const PanelDefinition& panel) {
  if (!mLink && mDevice) {
    // Reads only: it never claims the layer's status
    mLink = std::make_unique<HostLink::Client>(mDevice.get(), HostLink::ClientOptions {.mWritesStatus = false});
  }
  const float width = ImGui::GetContentRegionAvail().x;
  const float maxHeight = PreviewMaxHeight * mScale;
  const auto placeholder = [&](const char* text) {
    const ImVec2 size {width, maxHeight * 0.5f};
    const auto min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImGui::GetWindowDrawList()->AddRect(min, {min.x + size.x, min.y + size.y}, ImGui::GetColorU32(ImGuiCol_Border));
    const auto textSize = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText({min.x + (size.x - textSize.x) / 2, min.y + (size.y - textSize.y) / 2},
      ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
  };
  if (!mLink || !mLink->IsUsable()) {
    placeholder("No preview on this graphics card");
    return;
  }
  mLink->Update(TimeUtil::Now());
  const SharedAbi::PanelEntry* entry = nullptr;
  for (const auto& candidate: mLink->Panels()) {
    if (candidate.mPanelID == panel.mID) {
      entry = &candidate;
    }
  }
  mPreviewPanel = panel.mID;
  mPreviewDrawn = true;
  SetTimer(mWindow, PreviewTimer, PreviewCheckMs, nullptr);
  if (!mLink->IsConnected()) {
    placeholder("Starting...");
    return;
  }
  if (!entry || !(entry->mFlags & SharedAbi::PanelFlags::Visible) || entry->mContentWidth == 0
      || entry->mCurrentSlot >= SharedAbi::RingSlots) {
    placeholder(panel.mEnabled ? (mPanels.IsHidden(panel.mID) ? "Hidden for now" : "Waiting for it to show...") : "Turned off");
    return;
  }
  bool budget = true;// one open a frame at most, like the layer
  const auto* resources = mLink->Resources(*entry, budget);
  const auto& texture = resources ? resources->mTextures[entry->mCurrentSlot] : winrt::com_ptr<ID3D11Texture2D> {};
  if (!texture) {
    placeholder("No preview (the game may use another graphics card)");
    return;
  }
  // Wait for that frame on the GPU, never here
  if (const auto context = mContext.try_as<ID3D11DeviceContext4>(); context && resources->mFence) {
    context->Wait(resources->mFence.get(), entry->mFenceValue);
  }
  if (mPreviewViews.size() > 24) {
    mPreviewViews.clear();// old generations' textures
  }
  auto& view = mPreviewViews[texture.get()];
  if (!view && FAILED(mDevice->CreateShaderResourceView(texture.get(), nullptr, view.put()))) {
    placeholder("No preview");
    return;
  }
  D3D11_TEXTURE2D_DESC desc {};
  texture->GetDesc(&desc);
  const float aspect = static_cast<float>(entry->mContentHeight) / static_cast<float>(entry->mContentWidth);
  float drawWidth = width;
  float drawHeight = width * aspect;
  if (drawHeight > maxHeight) {
    drawHeight = maxHeight;
    drawWidth = drawHeight / aspect;
  }
  ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(view.get())), {drawWidth, drawHeight}, {0, 0},
    {static_cast<float>(entry->mContentWidth) / static_cast<float>(desc.Width),
      static_cast<float>(entry->mContentHeight) / static_cast<float>(desc.Height)});
  mPreviewVersion = entry->mContentVersion;
  mPreviewShowing = true;
}

void SettingsWindow::CheckPreview() {
  // Stops once a frame has gone by without a preview (another tab)
  if (!mWindow || !mLink || !mPreviewDrawn) {
    if (mWindow) {
      KillTimer(mWindow, PreviewTimer);
    }
    return;
  }
  mLink->Update(TimeUtil::Now());
  for (const auto& entry: mLink->Panels()) {
    if (entry.mPanelID == mPreviewPanel && entry.mContentVersion != mPreviewVersion) {
      KeepPainting(1);// a new frame to show
      return;
    }
  }
}

void SettingsWindow::StartThumbnails() {
  if (mThumbnailTask.Started()) {
    return;// still taking the last ones: those will do
  }
  std::vector<HWND> windows;
  for (const auto& window: mWindows) {
    if (windows.size() >= 60) {
      break;
    }
    windows.push_back(window.mWindow);
  }
  const float size = PickerThumbnailHeight * mScale;
  const auto maxWidth = static_cast<uint32_t>(size * 16 / 9);
  const auto maxHeight = static_cast<uint32_t>(size);
  mThumbnailTask = ThreadUtil::BackgroundTask<Thumbnails>::Start(L"Window pictures", [windows, maxWidth, maxHeight] {
    Thumbnails taken;
    for (const auto window: windows) {
      if (auto thumbnail = WindowUtil::GrabThumbnail(window, maxWidth, maxHeight)) {
        taken.emplace_back(window, std::move(*thumbnail));
      }
    }
    return taken;
  });
}

void SettingsWindow::TakeThumbnails() {
  if (!mThumbnailTask.Started()) {
    return;
  }
  if (mThumbnailTask.Running()) {
    KeepPainting(2);// they appear as soon as they're ready
    return;
  }
  for (auto& [window, thumbnail]: mThumbnailTask.Take().value_or(Thumbnails {})) {
    const D3D11_TEXTURE2D_DESC desc {
      .Width = thumbnail.mWidth,
      .Height = thumbnail.mHeight,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .SampleDesc = {1, 0},
      .Usage = D3D11_USAGE_IMMUTABLE,
      .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    const D3D11_SUBRESOURCE_DATA data {thumbnail.mBgra.data(), thumbnail.mWidth * 4, 0};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    if (mDevice && SUCCEEDED(mDevice->CreateTexture2D(&desc, &data, texture.put()))
        && SUCCEEDED(mDevice->CreateShaderResourceView(texture.get(), nullptr, view.put()))) {
      mThumbnails[window] = std::move(view);
    }
  }
  KeepPainting(TailFrames);
}

void SettingsWindow::WindowPicker() {
  constexpr const char* Title = "Pick a window";
  if (mOpenPicker) {
    mOpenPicker = false;
    ImGui::OpenPopup(Title);
    mFilter.fill(0);
    mWindows.clear();
    mThumbnails.clear();
    mPickerSelection = -1;
    StartListing();
  }
  const auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
  ImGui::SetNextWindowSize({viewport->WorkSize.x * 0.82f, viewport->WorkSize.y * 0.82f}, ImGuiCond_Appearing);
  bool open = true;
  if (!ImGui::BeginPopupModal(Title, &open, ImGuiWindowFlags_NoSavedSettings)) {
    return;
  }
  ImGui::TextUnformatted(
    mPickerRetargets ? "Which window should this panel show?" : "Which window do you want to see in VR?");
  const float refreshWidth = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetNextItemWidth(-(refreshWidth + ImGui::GetStyle().ItemSpacing.x));
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::InputTextWithHint("##search", "Type to search", mFilter.data(), mFilter.size());
  ImGui::SameLine();
  ImGui::BeginDisabled(mListing.Started());
  if (ImGui::Button("Refresh")) {
    mWindows.clear();
    mThumbnails.clear();
    mPickerSelection = -1;
    StartListing();
  }
  ImGui::EndDisabled();

  int chosen = -1;
  const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
  constexpr auto tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter;
  if (ImGui::BeginTable("windows", 2, tableFlags, {0, -footer})) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Program", ImGuiTableColumnFlags_WidthFixed, PickerProgramColumn * mScale);
    ImGui::TableHeadersRow();
    const auto filter = StringUtil::Widen(mFilter.data());
    for (int i = 0; i < static_cast<int>(mWindows.size()); ++i) {
      const auto& window = mWindows[i];
      const auto program = WindowUtil::ExecutableName(window);
      if (!StringUtil::ContainsIgnoreCase(window.mTitle, filter) && !StringUtil::ContainsIgnoreCase(program, filter)) {
        continue;
      }
      const float thumbHeight = PickerThumbnailHeight * mScale;
      const float thumbWidth = thumbHeight * 16 / 9;
      ImGui::TableNextRow(ImGuiTableRowFlags_None, thumbHeight + ImGui::GetStyle().CellPadding.y * 2);
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      constexpr auto rowFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick
        | ImGuiSelectableFlags_AllowOverlap;
      const auto rowTop = ImGui::GetCursorPos();
      if (ImGui::Selectable("##window", i == mPickerSelection, rowFlags, {0, thumbHeight})) {
        mPickerSelection = i;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          chosen = i;
        }
      }
      // Its picture (or an empty frame until it's taken), then its title
      ImGui::SetCursorPos(rowTop);
      if (const auto thumbnail = mThumbnails.find(window.mWindow); thumbnail != mThumbnails.end()) {
        D3D11_TEXTURE2D_DESC desc {};
        winrt::com_ptr<ID3D11Resource> resource;
        thumbnail->second->GetResource(resource.put());
        resource.as<ID3D11Texture2D>()->GetDesc(&desc);
        const float scale = std::min(thumbWidth / desc.Width, thumbHeight / desc.Height);
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(thumbnail->second.get())),
          {desc.Width * scale, desc.Height * scale});
      } else {
        ImGui::Dummy({thumbWidth, thumbHeight});
        const auto min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddRect(min, {min.x + thumbWidth, min.y + thumbHeight},
          ImGui::GetColorU32(ImGuiCol_Border));
      }
      ImGui::SetCursorPos({rowTop.x + thumbWidth + ImGui::GetStyle().ItemSpacing.x,
        rowTop.y + (thumbHeight - ImGui::GetTextLineHeight()) / 2});
      ImGui::TextUnformatted(Utf8(window.mTitle).c_str());
      ImGui::PopID();
      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (thumbHeight - ImGui::GetTextLineHeight()) / 2);
      ImGui::TextDisabled("%s", Utf8(program).c_str());
    }
    ImGui::EndTable();
  }
  if (mListing.Started()) {
    ImGui::TextDisabled("Looking for windows...");
  } else if (mWindows.empty()) {
    ImGui::TextDisabled("No windows to show. Open the program first, then choose Refresh.");
  } else {
    ImGui::BeginDisabled(mPickerSelection < 0);
    if (ImGui::Button(mPickerRetargets ? "Use this window" : "Show it in VR")) {
      chosen = mPickerSelection;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
  }
  if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  if (chosen >= 0 && chosen < static_cast<int>(mWindows.size())) {
    Pick(mWindows[chosen]);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void SettingsWindow::Pick(const WindowUtil::WindowInfo& window) {
  if (!mPickerRetargets) {
    mSelected = mPanels.AddWindow(window, false);
    return;
  }
  CommitTextFields();
  auto panel = mPanels.Find(mSelected);
  if (!panel || panel->mKind != PanelKind::Window) {
    return;// DU-2: gone while the picker was open
  }
  panel->mWindow = WindowMatch::FromWindow(window);
  mPanels.Update(*panel);
  mFieldsFor = 0;// reload the fields from the new rule
  Log::Info("Panels: '{}' now shows '{}'", Utf8(panel->mName), Utf8(window.mTitle));
}

// ---- Keybinds (doc 09 "Keybinds", "Bind UI") ---------------------------

// Hands (doc 05): stored in "hands"
void SettingsWindow::HandsPage() {
  if (!ImGui::BeginChild("hands", {0, 0}, ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  auto hands = mPanels.Hands();
  ImGui::PushItemWidth(ImGui::GetFontSize() * 14);
  ImGui::Checkbox("Use my hands on panels", &hands.mEnabled);
  Help(
    "Uses your headset's hand tracking: put the controllers down. Point with your index finger to tap; use two "
    "fingers to scroll. Each panel can turn these off (Panels tab, Hands).");
  ImGui::BeginDisabled(!hands.mEnabled);
  ImGui::Checkbox("Show my hand while pointing", &hands.mShowWhilePointing);
  Help("A see-through hand appears while you point, so you can see where you'll touch.");
  ImGui::SliderFloat("Move taps up", &hands.mTapOffsetUpMillimeters, -HandSettings::MaxTapOffsetMillimeters,
    HandSettings::MaxTapOffsetMillimeters, "%.0f mm", Clamp);
  Help("If your taps land below where you aim, move them up. Below zero moves them down.");
  ImGui::Checkbox("Reverse two-finger scrolling", &hands.mReverseScroll);
  Help("Normally what's on the panel follows your fingers, like a phone. Tick this for the other way.");
  ImGui::EndDisabled();
  ImGui::PopItemWidth();
  // Whole millimetres: the slider's steps
  hands.mTapOffsetUpMillimeters = std::round(hands.mTapOffsetUpMillimeters);
  if (hands != mPanels.Hands()) {
    mPanels.SetHands(hands);
  }
  KeyboardSettingsUi(hands.mEnabled);
  ImGui::EndChild();
}

// ---- General: performance, games, startup, about --------------------------------

void SettingsWindow::GeneralPage() {
  if (!ImGui::BeginChild("general", {0, 0}, ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  ImGui::PushItemWidth(ImGui::GetFontSize() * 14);

  // PN-12
  ImGui::SeparatorText("Performance");
  const int current = static_cast<int>(std::lround(mPanels.MaxUpdatesPerSecond()));
  if (ImGui::BeginCombo("Panels update at most", std::format("{} times a second", current).c_str())) {
    for (const int rate: PanelManager::UpdateRates) {
      const auto label = std::format("{} times a second{}", rate, rate == 30 ? " (the default)" : "");
      if (ImGui::Selectable(label.c_str(), rate == current)) {
        mPanels.SetMaxUpdatesPerSecond(rate);
      }
    }
    ImGui::EndCombo();
  }
  Help("A panel only updates when what it shows changes, and never more often than this. Lower is lighter on your PC; "
       "30 suits most. A panel can choose fewer (Panels tab).");

  // VR-8: the layer, everywhere or not in some games
  ImGui::Spacing();
  ImGui::SeparatorText("Games");
  auto games = mPanels.Games();
  bool gamesChanged = ImGui::Checkbox("Use Open Desktop VR in games", &games.mLayerEnabled);
  Help("Off, games run as if this program weren't installed. Takes effect when a game starts.");
  ImGui::BeginDisabled(!games.mLayerEnabled);
  ImGui::TextDisabled("Not in these games (takes effect when they start):");
  for (std::size_t i = 0; i < games.mDisabledGames.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      games.mDisabledGames.erase(games.mDisabledGames.begin() + static_cast<std::ptrdiff_t>(i));
      gamesChanged = true;
      ImGui::PopID();
      break;
    }
    ImGui::SetItemTooltip("Show panels in this game again");
    ImGui::SameLine();
    ImGui::TextUnformatted(Utf8(games.mDisabledGames[i]).c_str());
    ImGui::PopID();
  }
  if (games.mDisabledGames.empty()) {
    ImGui::TextDisabled("None.");
  }
  const auto& running = mPanels.AttachedGame();
  const bool listed = std::ranges::any_of(
    games.mDisabledGames, [&](const auto& game) { return StringUtil::EqualsIgnoreCase(game, running); });
  if (!running.empty() && !listed && ImGui::Button(std::format("Not in {} (running now)", Utf8(running)).c_str())) {
    games.mDisabledGames.push_back(running);
    gamesChanged = true;
  }
  const bool entered = ImGui::InputTextWithHint("##disabledGame", "A game's .exe name", mNewDisabledGame.data(),
    mNewDisabledGame.size(), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if ((ImGui::Button("Add##disabledGame") || entered) && mNewDisabledGame[0] != '\0') {
    const auto file = std::filesystem::path {StringUtil::Widen(mNewDisabledGame.data())}.filename().wstring();
    if (!file.empty()) {
      games.mDisabledGames.push_back(file);
      gamesChanged = true;
    }
    mNewDisabledGame[0] = '\0';
  }
  ImGui::EndDisabled();
  if (gamesChanged) {
    mPanels.SetGames(games);
  }

  // UI-7
  ImGui::Spacing();
  ImGui::SeparatorText("Startup");
  const auto exe = ProcessUtil::ExecutablePath();
  if (!mStartWithWindows) {
    mStartWithWindows = Autostart::IsEnabled(exe);
  }
  bool start = *mStartWithWindows;
  if (ImGui::Checkbox("Start with Windows", &start)) {
    if (Autostart::SetEnabled(start, exe)) {
      Log::Info("Start with Windows: {}", start ? "on" : "off");
    } else {
      Log::Warning("Start with Windows couldn't be turned {}", start ? "on" : "off");
    }
    mStartWithWindows = Autostart::IsEnabled(exe);
  }
  Help("Starts in the tray when you sign in to Windows, ready before the game.");

  // About
  ImGui::Spacing();
  ImGui::SeparatorText("About");
  ImGui::TextUnformatted(Utf8(Product::DisplayName).c_str());
  ImGui::TextDisabled("%s", Product::BuildDescription().c_str());
  ImGui::TextDisabled("Settings and logs: %s", Utf8(Product::DataFolder().wstring()).c_str());
  if (ImGui::Button("Open the logs folder")) {
    const auto folder = Product::LogsFolder();
    ShellExecuteW(mWindow, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::PopItemWidth();
  ImGui::EndChild();
}

// ---- Diagnostics (M6c, doc 09) -------------------------------------------------

namespace {

ImVec4 StatusColor(Diagnostics::Verdict verdict) {
  const auto rgb = [verdict] {
    switch (verdict) {
      case Diagnostics::Verdict::Pass:
        return Theme::StatusGood;
      case Diagnostics::Verdict::Warning:
        return Theme::StatusWarning;
      case Diagnostics::Verdict::Fail:
        return Theme::StatusProblem;
      default:
        return Theme::TextDim;
    }
  }();
  const auto c = Theme::ToRgba(rgb);
  return {c.r, c.g, c.b, c.a};
}

ImVec4 ToImVec4(uint32_t rgb) {
  const auto c = Theme::ToRgba(rgb);
  return {c.r, c.g, c.b, c.a};
}

// A round mark in the verdict's colour, the height of a line of text
void StatusMark(Diagnostics::Verdict verdict) {
  const float size = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled({at.x + size / 2, at.y + size / 2}, size * 0.3f, ImGui::GetColorU32(StatusColor(verdict)));
  ImGui::Dummy({size, size});
}

void Rows(const char* id, const std::vector<Host::Support::Row>& rows) {
  if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.35f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.65f);
    for (const auto& row: rows) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("%s", row.mLabel.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextWrapped("%s", row.mValue.c_str());
    }
    ImGui::EndTable();
  }
}

}// namespace

void SettingsWindow::DiagnosticsPage() {
  mDiagnosticsDrawn = true;
  if (!mDiagnosticsWasDrawn) {
    mSelfTestWanted = true;// just came into view: fresh checks
  }
  if (mWindow && !mDiagnosticsTimer) {
    SetTimer(mWindow, DiagnosticsTimer, DiagnosticsCheckMs, nullptr);
    mDiagnosticsTimer = true;
  }
  if (mSelfTestWanted) {
    StartSelfTest();
  }
  if (!ImGui::BeginChild("diagnostics", {0, 0}, ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  SelfTestSection();

  const auto stats = mRenderer.Stats();
  mShownGame = stats.mLayerStatus ? stats.mLayerStatus->mLayerProcessID : 0;
  mShownSession = stats.mLayerStatus ? stats.mLayerStatus->mSessionState : 0;
  mShownFrames = stats.mLayerStatus ? stats.mLayerStatus->mFramesSinceAttach : 0;
  ImGui::Spacing();
  ImGui::SeparatorText("In game");
  if (const auto rows = Support::DescribeGame(stats); rows.empty()) {
    ImGui::TextWrapped("No game has loaded the layer since Open Desktop VR started.");
  } else {
    Rows("game", rows);
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Apps");
  if (const auto rows = Support::DescribeHelpers(mPanels, mAppHelper); rows.empty()) {
    ImGui::TextDisabled("No window panels.");
  } else {
    Rows("apps", rows);
  }

  // Doc 09: A/B switches for performance reports
  ImGui::Spacing();
  ImGui::SeparatorText("Compare performance");
  ImGui::TextWrapped("Turn these off and on while watching the game's frame rate, to see what Open Desktop VR costs:");
  bool showing = mPanels.AnyShowing();
  if (ImGui::Checkbox("Show panels in VR##ab", &showing)) {
    Log::Info("Panels {} (Diagnostics tab)", mPanels.ToggleAll() ? "shown" : "hidden");
  }
  ImGui::SameLine();
  auto hands = mPanels.Hands();
  if (ImGui::Checkbox("Use my hands on panels##ab", &hands.mEnabled)) {
    mPanels.SetHands(hands);
    Log::Info("Hands on panels: {} (Diagnostics tab)", hands.mEnabled ? "on" : "off");
  }

  LogSection();
  SupportSection();
  ImGui::EndChild();
}

void SettingsWindow::SelfTestSection() {
  ImGui::SeparatorText("Panels don't show in the game?");
  ImGui::BeginDisabled(mSelfTestTask.Started());
  if (ImGui::Button(mSelfTestTask.Started() ? "Checking..." : "Check again")) {
    mSelfTestWanted = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("Start Open Desktop VR before the game for the most to check.");
  if (!mSelfTest) {
    return;
  }
  const float indent = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x;
  for (const auto& check: *mSelfTest) {
    ImGui::PushID(static_cast<int>(check.mID));
    StatusMark(check.mVerdict);
    ImGui::SameLine();
    ImGui::TextUnformatted(check.mTitle.c_str());
    ImGui::Indent(indent);
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(Theme::TextDim));
    ImGui::TextWrapped("%s", check.mDetail.c_str());
    ImGui::PopStyleColor();
    if (!check.mFix.empty()) {
      ImGui::TextWrapped("%s", check.mFix.c_str());
    }
    if (check.mID == Diagnostics::CheckID::Recenter && ImGui::Button("Bring panels in front of me")) {
      mRenderer.Recenter();
      Log::Info("Recentered from the Diagnostics tab");
    }
    ImGui::Unindent(indent);
    ImGui::PopID();
  }
}

void SettingsWindow::LogSection() {
  ImGui::Spacing();
  ImGui::SeparatorText("Log");
  if (const auto version = mLogRing.Version(); version != mLogVersion) {
    mLogVersion = version;
    mLogLines = mLogRing.Lines();
    mLogScrollToEnd = true;
  }
  const float height = ImGui::GetTextLineHeightWithSpacing() * 12;
  if (ImGui::BeginChild("log", {0, height}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
    const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(mLogLines.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& line = mLogLines[static_cast<std::size_t>(i)];
        const bool marked = line.mLevel >= Log::Level::Warning;
        if (marked) {
          ImGui::PushStyleColor(
            ImGuiCol_Text, ToImVec4(line.mLevel == Log::Level::Error ? Theme::StatusProblem : Theme::StatusWarning));
        }
        ImGui::TextUnformatted(line.mText.c_str());
        if (marked) {
          ImGui::PopStyleColor();
        }
      }
    }
    // New lines: follow them, unless scrolled up to read
    if (std::exchange(mLogScrollToEnd, false) && (atEnd || ImGui::GetScrollMaxY() == 0)) {
      ImGui::SetScrollHereY(1.0f);
    }
  }
  ImGui::EndChild();
  if (ImGui::Button("Copy")) {
    ImGui::SetClipboardText(mLogRing.Text().c_str());
  }
  ImGui::SetItemTooltip("Copies these lines, to paste into a message");
  ImGui::SameLine();
  if (ImGui::Button("Open the logs folder##diagnostics")) {
    const auto folder = Product::LogsFolder();
    ShellExecuteW(mWindow, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("The newest %zu lines; the files hold more.", mLogRing.Capacity());
}

void SettingsWindow::SupportSection() {
  ImGui::Spacing();
  ImGui::SeparatorText("Support");
  ImGui::BeginDisabled(mBundleTask.Started() || mPendingDialog.has_value());
  if (ImGui::Button(mBundleTask.Started() ? "Saving..." : "Save debug info...")) {
    mOpenPrivacy = true;
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("One file with what helps find a problem, to send with a report");
  ImGui::SameLine();
  if (ImGui::Button("Open the mirror")) {
    const auto mirror = ProcessUtil::ExecutablePath().parent_path() / L"odvr-mirror.exe";
    ShellExecuteW(mWindow, L"open", mirror.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::SetItemTooltip("Shows panels on the desktop, as the game gets them");
  if (mBundleTask.Started()) {
    if (!mBundleTask.Running()) {
      mBundleResult = mBundleTask.Take().value_or(Diagnostics::BundleResult {false, 0, 0, "unexpected error"});
    } else {
      KeepPainting(2);
    }
  }
  if (mBundleResult) {
    if (mBundleResult->mOk) {
      ImGui::TextWrapped("Saved %s (%zu files, %zu KB).", Utf8(mBundlePath.filename().wstring()).c_str(), mBundleResult->mFiles,
        mBundleResult->mBytes / 1024);
      ImGui::SameLine();
      if (ImGui::SmallButton("Show it")) {
        const auto select = std::format(L"/select,\"{}\"", mBundlePath.wstring());
        ShellExecuteW(mWindow, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
      }
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(Theme::StatusProblem));
      ImGui::TextWrapped("Couldn't save it: %s", mBundleResult->mError.c_str());
      ImGui::PopStyleColor();
    }
  }
}

void SettingsWindow::PrivacyPopup() {
  constexpr const char* Title = "Before you save";
  if (std::exchange(mOpenPrivacy, false)) {
    ImGui::OpenPopup(Title);
  }
  ImGui::SetNextWindowSize({ImGui::GetFontSize() * 30, 0});
  if (!ImGui::BeginPopupModal(Title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
    return;
  }
  // Doc 09: a short disclaimer before saving
  ImGui::TextWrapped("%s", std::string {Diagnostics::PrivacyNotice()}.c_str());
  ImGui::Spacing();
  if (ImGui::Button("Choose where to save...") && !mPendingDialog) {
    mPendingDialog = PendingDialog {true, {}, Diagnostics::SuggestedBundleName(), true};
    PostMessageW(mWindow, FileDialogMessage, 0, 0);// once this frame is done (round 15)
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void SettingsWindow::StartSelfTest() {
  mSelfTestWanted = false;
  if (mSelfTestTask.Started()) {
    return;
  }
  const auto stats = mRenderer.Stats();
  auto input = Support::FromHost(stats, mPanels);
  const uint32_t game = stats.mLayerStatus ? stats.mLayerStatus->mLayerProcessID : 0;
  mSelfTestTask = ThreadUtil::BackgroundTask<std::vector<Diagnostics::SelfCheck>>::Start(L"Self-test", [input, game]() mutable {
    Support::AddSystem(input, game);
    return Diagnostics::RunSelfTest(input);
  });
  KeepPainting(2);
}

void SettingsWindow::TakeSelfTest() {
  if (!mSelfTestTask.Started()) {
    return;
  }
  if (mSelfTestTask.Running()) {
    KeepPainting(2);
    return;
  }
  if (auto result = mSelfTestTask.Take()) {
    mSelfTest = std::move(*result);
    std::size_t problems = 0;
    for (const auto& check: *mSelfTest) {
      problems += check.mVerdict == Diagnostics::Verdict::Fail || check.mVerdict == Diagnostics::Verdict::Warning;
    }
    Log::Info("Self-test: {} checks, {} to look at", mSelfTest->size(), problems);
  }
  KeepPainting(TailFrames);
}

void SettingsWindow::StartBundle(std::filesystem::path zip) {
  if (mBundleTask.Started()) {
    return;
  }
  const auto stats = mRenderer.Stats();
  Support::BundleRequest request;
  request.mZip = zip;
  request.mSelfTest = Support::FromHost(stats, mPanels);
  request.mGameProcessID = stats.mLayerStatus ? stats.mLayerStatus->mLayerProcessID : 0;
  request.mStatus = Support::FormatRows(Support::DescribeGame(stats));
  request.mHelpers = Support::FormatRows(Support::DescribeHelpers(mPanels, mAppHelper));
  request.mRecentLog = mLogRing.Text();
  mBundlePath = zip;
  mBundleResult.reset();
  mBundleTask = ThreadUtil::BackgroundTask<Diagnostics::BundleResult>::Start(
    L"Debug info", [request = std::move(request)]() mutable { return Support::SaveBundle(std::move(request)); });
  KeepPainting(2);
}

void SettingsWindow::CheckDiagnostics() {
  // Stops once a frame has gone by without the tab
  if (!mWindow || !mDiagnosticsDrawn) {
    if (mWindow) {
      KillTimer(mWindow, DiagnosticsTimer);
    }
    mDiagnosticsTimer = false;
    return;
  }
  const auto stats = mRenderer.Stats();
  const uint32_t game = stats.mLayerStatus ? stats.mLayerStatus->mLayerProcessID : 0;
  const uint32_t session = stats.mLayerStatus ? stats.mLayerStatus->mSessionState : 0;
  const uint64_t frames = stats.mLayerStatus ? stats.mLayerStatus->mFramesSinceAttach : 0;
  if (mLogRing.Version() != mLogVersion || game != mShownGame || session != mShownSession || frames != mShownFrames) {
    KeepPainting(1);
  }
}

// ---- Profiles (ADR-038) ------------------------------------------------------

std::optional<std::filesystem::path> SettingsWindow::AskProfileFile(bool save, const std::wstring& name, bool debugInfo) {
  // The file dialogs need COM on this (the UI) thread
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  struct Uninit {
    bool mOwned;
    ~Uninit() {
      if (mOwned) {
        CoUninitialize();
      }
    }
  } uninit {SUCCEEDED(com)};
  winrt::com_ptr<IFileDialog> dialog;
  if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.put())))) {
    return std::nullopt;
  }
  const COMDLG_FILTERSPEC profileTypes[] {{L"Open Desktop VR profile", L"*.json"}, {L"All files", L"*.*"}};
  const COMDLG_FILTERSPEC zipTypes[] {{L"Zip file", L"*.zip"}};
  if (debugInfo) {
    dialog->SetFileTypes(static_cast<UINT>(std::size(zipTypes)), zipTypes);
    dialog->SetDefaultExtension(L"zip");
  } else {
    dialog->SetFileTypes(static_cast<UINT>(std::size(profileTypes)), profileTypes);
    dialog->SetDefaultExtension(L"json");
  }
  if (save) {
    dialog->SetFileName(name.c_str());
  }
  // Modal on this window: nothing else here runs a dialog meanwhile (DU-1)
  if (FAILED(dialog->Show(mWindow))) {
    return std::nullopt;// cancelled, or failed: the same (DU-1)
  }
  winrt::com_ptr<IShellItem> item;
  PWSTR path = nullptr;
  if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
    return std::nullopt;
  }
  std::filesystem::path result {path};
  CoTaskMemFree(path);
  return result;
}

void SettingsWindow::RunPendingDialog() {
  const auto pending = std::exchange(mPendingDialog, std::nullopt);
  if (!pending || !mWindow) {
    return;
  }
  if (pending->mDebugInfo) {
    if (const auto file = AskProfileFile(true, pending->mName, true)) {
      StartBundle(*file);
    }
  } else if (pending->mSave) {
    if (const auto file = AskProfileFile(true, pending->mName)) {
      mPanels.ExportProfile(pending->mProfileID, *file);// by ID: it may be gone by now (DU-2)
    }
  } else if (const auto file = AskProfileFile(false, {})) {
    if (const auto id = mPanels.ImportProfile(*file)) {
      mSelectedProfile = *id;
    }
  }
  KeepPainting(TailFrames);
}

void SettingsWindow::ProfilesPage() {
  if (!ImGui::BeginChild("profiles", {0, 0}, ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  const auto profiles = mPanels.Profiles();
  const auto& active = mPanels.ActiveProfile();
  if (std::ranges::find(profiles, mSelectedProfile, &PanelManager::ProfileSummary::mID) == profiles.end()) {
    mSelectedProfile = active;// gone (DU-2), or nothing chosen yet
  }
  ImGui::TextDisabled("Each profile is its own set of panels. A profile can switch itself on when its game starts.");
  ImGui::Spacing();

  // The list, the one in use ticked
  const float width = ListWidth * mScale;
  ImGui::BeginGroup();
  if (ImGui::BeginListBox("##profiles", {width, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()})) {
    for (const auto& profile: profiles) {
      ImGui::PushID(profile.mID.c_str());
      const auto label = std::format("{}{}", Utf8(profile.mName), profile.mID == active ? "   (in use)" : "");
      if (ImGui::Selectable(label.c_str(), profile.mID == mSelectedProfile)) {
        mSelectedProfile = profile.mID;
      }
      ImGui::SetItemTooltip("%s", std::format("{} panel{}{}", profile.mPanelCount, profile.mPanelCount == 1 ? "" : "s",
        profile.mID == active ? ", in use" : "").c_str());
      ImGui::PopID();
    }
    ImGui::EndListBox();
  }
  if (ImGui::Button("Import...") && !mPendingDialog) {
    // Opened once this frame is done (round 15)
    mPendingDialog = PendingDialog {false, {}, {}};
    PostMessageW(mWindow, FileDialogMessage, 0, 0);
  }
  ImGui::SetItemTooltip("Adds a profile from a file someone exported");
  ImGui::EndGroup();
  ImGui::SameLine();

  // The selected profile
  ImGui::BeginChild("profile", {0, 0});
  const auto selected = std::ranges::find(profiles, mSelectedProfile, &PanelManager::ProfileSummary::mID);
  if (selected != profiles.end()) {
    const auto& profile = *selected;
    if (mProfileNameFor != profile.mID) {
      mProfileNameFor = profile.mID;
      StringUtil::CopyTruncated(Utf8(profile.mName), mProfileName);
    }
    ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputText("Name", mProfileName.data(), mProfileName.size());
    if (ImGui::IsItemDeactivatedAfterEdit() && mProfileName[0] != '\0') {
      mPanels.RenameProfile(profile.mID, StringUtil::Widen(mProfileName.data()));
    }
    ImGui::PopItemWidth();
    ImGui::Text("%zu panel%s", profile.mPanelCount, profile.mPanelCount == 1 ? "" : "s");
    if (profile.mID == active) {
      ImGui::SameLine();
      ImGui::TextDisabled("(in use)");
    } else {
      ImGui::SameLine();
      if (ImGui::Button("Use this profile")) {
        mPanels.SwitchProfile(profile.mID, "settings window");
      }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Games");
    ImGui::TextDisabled("When one of these games starts, this profile is used; when it ends, the one before comes back.");
    auto games = profile.mGames;
    bool gamesChanged = false;
    for (std::size_t i = 0; i < games.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("x")) {
        games.erase(games.begin() + static_cast<std::ptrdiff_t>(i));
        gamesChanged = true;
        ImGui::PopID();
        break;
      }
      ImGui::SetItemTooltip("Stop using this profile for this game");
      ImGui::SameLine();
      ImGui::TextUnformatted(Utf8(games[i]).c_str());
      ImGui::PopID();
    }
    if (games.empty()) {
      ImGui::TextDisabled("None: you switch to it yourself.");
    }
    const auto& running = mPanels.AttachedGame();
    const bool listed = std::ranges::any_of(games, [&](const auto& game) { return StringUtil::EqualsIgnoreCase(game, running); });
    if (!running.empty() && !listed) {
      if (ImGui::Button(std::format("Add {} (running now)", Utf8(running)).c_str())) {
        games.push_back(running);
        gamesChanged = true;
      }
    }
    ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
    const bool entered = ImGui::InputTextWithHint("##game", "A game's .exe name", mNewGame.data(), mNewGame.size(),
      ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if ((ImGui::Button("Add") || entered) && mNewGame[0] != '\0') {
      const auto file = std::filesystem::path {StringUtil::Widen(mNewGame.data())}.filename().wstring();
      if (!file.empty()) {
        games.push_back(file);
        gamesChanged = true;
      }
      mNewGame[0] = '\0';
    }
    if (gamesChanged) {
      mPanels.SetProfileGames(profile.mID, std::move(games));
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Profile");
    if (ImGui::Button("New profile")) {
      mSelectedProfile = mPanels.NewProfile(L"New profile");
    }
    ImGui::SetItemTooltip("A new profile with a copy of the panels in use; switch to it to change them");
    ImGui::SameLine();
    if (ImGui::Button("Export...") && !mPendingDialog) {
      mPendingDialog = PendingDialog {true, profile.mID, profile.mName + L".json"};
      PostMessageW(mWindow, FileDialogMessage, 0, 0);
    }
    ImGui::SetItemTooltip("Saves this profile as one file, to share or keep");
    ImGui::SameLine();
    ImGui::BeginDisabled(profiles.size() < 2);
    if (ImGui::Button("Delete")) {
      mPanels.DeleteProfile(profile.mID);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", profiles.size() < 2 ? "The last profile can't be deleted" : "Deletes this profile and its panels");
  }
  ImGui::EndChild();
  ImGui::EndChild();
}

bool SettingsWindow::LayoutCombo(const char* label, std::string* klid, const char* none) {
  std::string preview = none;
  if (!klid->empty()) {
    preview = *klid;
    if (mLayouts) {
      for (const auto& layout: *mLayouts) {
        if (layout.mKLID == *klid) {
          preview = Utf8(layout.mDisplayName);
        }
      }
    }
  }
  bool changed = false;
  if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable(none, klid->empty())) {
      klid->clear();
      changed = true;
    }
    if (!mLayouts) {
      ImGui::TextDisabled("Reading your layouts...");
      KeepPainting(2);
    } else {
      for (const auto& layout: *mLayouts) {
        ImGui::PushID(layout.mKLID.c_str());
        if (ImGui::Selectable(Utf8(layout.mDisplayName).c_str(), layout.mKLID == *klid)) {
          *klid = layout.mKLID;
          changed = true;
        }
        ImGui::PopID();
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// The virtual keyboard (doc 07): stored in "keyboard"
void SettingsWindow::KeyboardSettingsUi(bool handsEnabled) {
  if (!mLayoutsTask.Started() && !mLayouts) {
    mLayoutsTask = ThreadUtil::BackgroundTask<std::vector<KeyboardLayout::LayoutInfo>>::Start(
      L"Keyboard layouts", [] { return KeyboardLayout::GetAvailableLayouts(); });
  }
  if (mLayoutsTask.Started() && !mLayoutsTask.Running()) {
    mLayouts = mLayoutsTask.Take().value_or(std::vector<KeyboardLayout::LayoutInfo> {});
  }
  if (mLayoutsTask.Running()) {
    KeepPainting(2);// the list appears when it's read
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Virtual keyboard");
  auto keyboard = mPanels.Keyboard();
  ImGui::PushItemWidth(ImGui::GetFontSize() * 14);
  ImGui::BeginDisabled(!handsEnabled);
  ImGui::Checkbox("Virtual keyboard", &keyboard.mEnabled);
  Help(
    "A keyboard you type on with your fingertips. It appears when you tap into a text field on a panel and hides "
    "when you tap somewhere else. A keybind can show or hide it too (Keybinds tab).");
  ImGui::BeginDisabled(!keyboard.mEnabled);
  LayoutCombo("Layout", &keyboard.mPrimaryLayout, "My Windows layouts");
  Help("\"My Windows layouts\" follows the layouts you have in Windows. The keyboard's Layout key switches between them.");
  if (!keyboard.mPrimaryLayout.empty()) {
    LayoutCombo("Second layout", &keyboard.mSecondaryLayout, "None");
    Help("The keyboard's Layout key switches between the two.");
  }
  ImGui::Checkbox("Function keys (F1 to F12)", &keyboard.mFunctionRow);
  float volume = keyboard.mClickVolume * 100.0f;
  if (ImGui::SliderFloat("Key click", &volume, 0.0f, 100.0f, volume <= 0 ? "Off" : "%.0f%%", Clamp)) {
    keyboard.mClickVolume = std::round(volume) / 100.0f;
  }
  ImGui::Checkbox("Hide after Enter", &keyboard.mHideAfterEnter);
  Help("Off by default: in chat apps Enter sends a message, and you usually carry on typing.");
  ImGui::SliderFloat("Distance from your eyes", &keyboard.mDistanceCentimeters, KeyboardSettings::MinDistance,
    KeyboardSettings::MaxDistance, "%.0f cm", Clamp);
  Help("Too close is hard to focus on. Far enough to rest your eyes, near enough to reach: about 45 to 55 cm.");
  ImGui::SliderFloat("Below your eyes", &keyboard.mHeightCentimeters, KeyboardSettings::MinHeight,
    KeyboardSettings::MaxHeight, "%.0f cm", Clamp);
  ImGui::SliderFloat("Tilt", &keyboard.mTiltDegrees, KeyboardSettings::MinTilt, KeyboardSettings::MaxTilt,
    "%.0f\xC2\xB0", Clamp);
  Help("0 stands it upright; more tilts it back like a laptop's keyboard.");
  ImGui::SliderFloat("Key size", &keyboard.mKeyMillimeters, KeyboardSettings::MinKey, KeyboardSettings::MaxKey,
    "%.0f mm", Clamp);
  Help("From one key to the next. Bigger keys are easier to hit; the keyboard gets wider.");
  DragPercent("Opacity while typing", &keyboard.mTypingOpacity);
  DragPercent("Opacity otherwise", &keyboard.mIdleOpacity);
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::PopItemWidth();
  // Whole steps, as the sliders show them
  keyboard.mDistanceCentimeters = std::round(keyboard.mDistanceCentimeters);
  keyboard.mHeightCentimeters = std::round(keyboard.mHeightCentimeters);
  keyboard.mTiltDegrees = std::round(keyboard.mTiltDegrees);
  keyboard.mKeyMillimeters = std::round(keyboard.mKeyMillimeters);
  if (!(keyboard == mPanels.Keyboard())) {
    mPanels.SetKeyboard(keyboard);
  }
}

void SettingsWindow::KeybindsPage(const std::vector<PanelDefinition>& panels) {
  if (!ImGui::BeginChild("keybinds", {0, 0}, ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  // What's watched
  ImGui::TextDisabled("%s", mKeybinds.KeyboardReady() ? "Keyboard: ready." : "Keyboard: not available (see the log).");
  const auto controllers = mKeybinds.Controllers();
  std::string names;
  for (const auto& controller: controllers) {
    names += (names.empty() ? "" : ", ") + controller.mName;
  }
  ImGui::TextDisabled("Game controllers: %s", names.empty() ? "none connected." : names.c_str());
  ImGui::TextDisabled("Keybinds work while the game has focus. F13 to F24 make good keys for Stream Deck and similar tools.");
  ImGui::Spacing();

  constexpr auto tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("bindings", 3, tableFlags)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Keybind", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Or", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableHeadersRow();
    const auto row = [&](const char* label, const Keybinds::Action& action, const Keybinds::Slots& slots, bool indent) {
      ImGui::PushID(static_cast<int>(action.mKind));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (indent) {
        ImGui::Indent();
      }
      ImGui::TextUnformatted(label);
      if (indent) {
        ImGui::Unindent();
      }
      for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        ImGui::TableNextColumn();
        SlotButton(action, slot, slots[slot]);
      }
      ImGui::PopID();
    };
    ImGui::PushID("global");
    for (const auto kind: Keybinds::GlobalActions) {
      row(Keybinds::ActionName(kind).c_str(), {kind}, mPanels.GlobalKeybinds(kind), false);
    }
    ImGui::PopID();
    for (const auto& panel: panels) {
      ImGui::PushID(reinterpret_cast<const void*>(static_cast<uintptr_t>(panel.mID)));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::PushFont(mFonts.mHeading);
      ImGui::TextUnformatted(Utf8(panel.mName.empty() ? L"(no name)" : panel.mName).c_str());
      ImGui::PopFont();
      for (std::size_t i = 0; i < Keybinds::PanelActions.size(); ++i) {
        const auto kind = Keybinds::PanelActions[i];
        row(Keybinds::ActionName(kind).c_str(), {kind, panel.mID}, panel.mKeybinds[i], true);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
}

void SettingsWindow::SlotButton(const Keybinds::Action& action, std::size_t slot, const Keybinds::Binding& binding) {
  ImGui::PushID(static_cast<int>(slot));
  const float clear = binding.IsEmpty() ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
  const auto label = binding.IsEmpty() ? std::string {"Not set"} : Keybinds::BindingName(binding);
  if (binding.IsEmpty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  }
  if (ImGui::Button(std::format("{}##set", label).c_str(), {ImGui::GetContentRegionAvail().x - clear, 0})) {
    StartCapture(action, slot);
  }
  if (binding.IsEmpty()) {
    ImGui::PopStyleColor();
  }
  ImGui::SetItemTooltip("Click, then press the keys or buttons you want");
  if (!binding.IsEmpty()) {
    ImGui::SameLine();
    if (ImGui::Button("x", {ImGui::GetFrameHeight(), 0})) {
      Assign(action, slot, {});
    }
    ImGui::SetItemTooltip("Remove this keybind");
  }
  ImGui::PopID();
}

void SettingsWindow::StartCapture(const Keybinds::Action& action, std::size_t slot) {
  mCapture = mKeybinds.StartCapture();
  mCaptureAction = action;
  mCaptureSlot = slot;
  mCaptureStarted = TimeUtil::Now();
  mCaptureDoneAt.reset();
  mConflict.reset();
  mOpenCapture = true;
  KeepPainting(TailFrames);
}

void SettingsWindow::CapturePopup() {
  constexpr const char* Title = "Set a keybind";
  constexpr auto Timeout = std::chrono::seconds(10);
  constexpr auto ShowFor = std::chrono::milliseconds(700);
  if (mOpenCapture) {
    mOpenCapture = false;
    ImGui::OpenPopup(Title);
  }
  if (!ImGui::BeginPopupModal(Title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (mCapture) {
      mCapture->Cancel();// closed some other way
      mCapture.reset();
    }
    return;
  }
  KeepPainting(2);// live while it's open: what's held, the countdown
  const auto close = [&] {
    if (mCapture) {
      mCapture->Cancel();
      mCapture.reset();
    }
    mConflict.reset();
    ImGui::CloseCurrentPopup();
  };
  const auto now = TimeUtil::Now();
  ImGui::TextUnformatted(mKeybinds.Describe(mCaptureAction).c_str());
  ImGui::Spacing();
  if (mConflict) {
    // Show conflicts (doc 09): used elsewhere, so move it here or keep it there
    ImGui::Text("%s is already used for \"%s\".", Keybinds::BindingName(*mConflict).c_str(),
      mKeybinds.Describe(mConflictWith).c_str());
    ImGui::TextUnformatted("Use it here instead?");
    ImGui::Spacing();
    if (ImGui::Button("Use it here")) {
      if (const auto slots = SlotsFor(mConflictWith)) {
        for (std::size_t i = 0; i < slots->size(); ++i) {
          if ((*slots)[i] == *mConflict) {
            Assign(mConflictWith, i, {});
          }
        }
      }
      Assign(mCaptureAction, mCaptureSlot, *mConflict);
      close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      close();
    }
  } else if (!mCapture) {
    close();
  } else if (const auto state = mCapture->State(); state == Keybinds::BindCapture::State::Cancelled) {
    close();
  } else if (state == Keybinds::BindCapture::State::Done) {
    // The whole combination, briefly, before it's set (doc 09)
    const auto binding = mCapture->Current();
    ImGui::PushFont(mFonts.mHeading);
    ImGui::TextUnformatted(Keybinds::BindingName(binding).c_str());
    ImGui::PopFont();
    if (!mCaptureDoneAt) {
      mCaptureDoneAt = now;
    } else if (now - *mCaptureDoneAt >= ShowFor) {
      if (const auto other = mKeybinds.UsedBy(binding, mCaptureAction)) {
        mConflict = binding;
        mConflictWith = *other;
      } else {
        Assign(mCaptureAction, mCaptureSlot, binding);
        close();
      }
    }
  } else {
    ImGui::TextUnformatted("Press the keys or buttons you want, then let go.");
    const auto held = mCapture->Current();
    ImGui::PushFont(mFonts.mHeading);
    ImGui::TextUnformatted(held.IsEmpty() ? " " : Keybinds::BindingName(held).c_str());
    ImGui::PopFont();
    if (mCapture->ModifierOnly()) {
      ImGui::TextDisabled("Ctrl, Shift, Alt or Windows on its own can't be a keybind: add another key.");
    }
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(Timeout - (now - mCaptureStarted)).count();
    ImGui::TextDisabled("Escape cancels. Closes by itself in %lld s.", static_cast<long long>(std::max<int64_t>(left, 0)));
    if (now - mCaptureStarted >= Timeout || ImGui::Button("Cancel")) {
      close();
    }
  }
  ImGui::EndPopup();
}

std::optional<Keybinds::Slots> SettingsWindow::SlotsFor(const Keybinds::Action& action) const {
  if (Keybinds::IsGlobal(action.mKind)) {
    return mPanels.GlobalKeybinds(action.mKind);
  }
  if (const auto panel = mPanels.Find(action.mPanelID)) {
    return panel->KeybindsFor(action.mKind);
  }
  return std::nullopt;
}

void SettingsWindow::Assign(const Keybinds::Action& action, std::size_t slot, const Keybinds::Binding& binding) {
  if (slot >= 2) {
    return;
  }
  if (Keybinds::IsGlobal(action.mKind)) {
    auto slots = mPanels.GlobalKeybinds(action.mKind);
    slots[slot] = binding;
    mPanels.SetGlobalKeybinds(action.mKind, slots);
  } else if (auto panel = mPanels.Find(action.mPanelID)) {// DU-2: it may be gone
    panel->KeybindsFor(action.mKind)[slot] = binding;
    mPanels.Update(*panel);
  } else {
    return;
  }
  Log::Info("Keybinds: {} {}", mKeybinds.Describe(action),
    binding.IsEmpty() ? std::string {"cleared"} : "set to " + Keybinds::BindingName(binding));
}

}// namespace OpenDesktopVR::Host
