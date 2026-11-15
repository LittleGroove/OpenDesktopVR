// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "SettingsWindow.hpp"

#include <OpenDesktopVR/ImageFile.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/Theme.hpp>
#include <OpenDesktopVR/UpdatePolicy.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <dwmapi.h>
#include <imgui.h>
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

constexpr UINT_PTR PaintTimer = 1;
constexpr UINT_PTR ScreenshotIdleStart = 2;
constexpr UINT_PTR ScreenshotIdleEnd = 3;
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

}// namespace

SettingsWindow::SettingsWindow(HINSTANCE instance, RenderLoop& renderer, PanelManager& panels)
  : mInstance(instance), mRenderer(renderer), mPanels(panels) {
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &SettingsWindow::WindowProc;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  // Making a device takes 30 ms or more (180 ms while the driver loads):
  // do it now, off this thread, so the window opens quickly
  try {
    mDeviceTask = ThreadUtil::BackgroundTask<Gpu>::Start(L"Settings device", [] { return CreateDevice(); });
  } catch (...) {
    ThreadUtil::ReportEscapedException("starting the settings device");
  }
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
  mFirstPaintLogged = false;
  // Other programs may have changed the file since it was loaded
  mPanels.Refresh();
  bool created = false;
  ThreadUtil::CatchAll("opening the settings window", [&] { created = Create(); });
  if (!created) {
    Destroy();
    Log::Warning("Settings window: couldn't be opened");
    return;
  }
  Log::Info("Settings window opened ({:.0f}% scale)", mScale * 100);
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

  // Its own device: nothing here can wait on the panel renderer's GPU work.
  // Usually made in the background at start (and kept), so opening the
  // window stays within doc 08's 50 ms.
  if (mDevice && mDevice->GetDeviceRemovedReason() != S_OK) {
    mDevice = nullptr;
    mContext = nullptr;
  }
  if (!mDevice) {
    if (!mDeviceTask.Running()) {
      if (auto gpu = mDeviceTask.Take(); gpu && gpu->mDevice) {
        mDevice = std::move(gpu->mDevice);
        mContext = std::move(gpu->mContext);
      }
    }
    if (!mDevice) {
      auto gpu = CreateDevice();// not ready (or failed): never wait for it
      mDevice = std::move(gpu.mDevice);
      mContext = std::move(gpu.mContext);
    }
  }
  if (!mDevice) {
    return false;
  }
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

  // One ImGui context, for this thread only (ADR-020)
  mImGui = ImGui::CreateContext();
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

  ShowWindow(mWindow, SW_SHOWNORMAL);
  SetForegroundWindow(mWindow);
  KeepPainting(TailFrames);
  return true;
}

void SettingsWindow::Destroy() {
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
  mFonts = {};
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
  if (!mWindow || !mImGui || !mSwapchain) {
    return;
  }
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
  if (mRebuildFonts) {
    UiTheme::Apply(mScale);
    mFonts = UiTheme::LoadFonts(mScale);
    ImGui_ImplDX11_InvalidateDeviceObjects();// the next frame rebuilds the font texture
    mRebuildFonts = false;
  }
  ImGui_ImplDX11_NewFrame();
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
    if (settled && mPaints >= ScreenshotAfterPaints && !mListing.Started()) {
      TakeScreenshot();// before Present: a flip-model buffer is undefined after it
    } else {
      KeepPainting(2);
    }
  }

  const HRESULT result = mSwapchain->Present(1, 0);
  if (!mFirstPaintLogged) {
    // Doc 08: opening the window takes at most 50 ms
    mFirstPaintLogged = true;
    Log::Info("Settings window: first frame {} ms after opening",
      std::chrono::duration_cast<std::chrono::milliseconds>(TimeUtil::Now() - mOpenedAt).count());
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
    std::ofstream {report} << std::format("idle paints: {}\ninput messages: {} (last {:#x})\n", idlePaints, inputs,
      mLastInput);
    PostQuitMessage(0);
  }
}

// ---- The interface ---------------------------------------------------------

void SettingsWindow::BuildUi() {
  // The picker's list, once the worker has it
  if (mListing.Started() && !mListing.Running()) {
    mWindows = mListing.Take().value_or(std::vector<WindowUtil::WindowInfo> {});
    mPickerSelection = -1;
    KeepPainting(TailFrames);
  }

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
    bool visible = mRenderer.PanelsVisible();
    if (ImGui::Checkbox(visibleLabel, &visible)) {
      mRenderer.SetPanelsVisible(visible);
      Log::Info("Panels {}", visible ? "shown" : "hidden");
    }
    ImGui::Separator();
    ImGui::Spacing();

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
    WindowPicker();
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
      draw->AddText({corner.x + inset, textTop + line}, ImGui::GetColorU32(ImGuiCol_TextDisabled), showing.c_str());
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
  ImGui::TextDisabled("From where your head was when you last recentered. Ctrl+click a slider to type.");
  changed |= ImGui::SliderFloat("Left / right", &panel.mPosition[0], -2.0f, 2.0f, "%.2f m", Clamp);
  changed |= ImGui::SliderFloat("Down / up", &panel.mPosition[1], -1.5f, 1.5f, "%.2f m", Clamp);
  float distance = -panel.mPosition[2];
  if (ImGui::SliderFloat("Distance", &distance, 0.2f, 3.0f, "%.2f m", Clamp)) {
    panel.mPosition[2] = -distance;
    changed = true;
  }
  changed |= ImGui::SliderFloat("Width", &panel.mWidthMeters, 0.1f, 2.0f, "%.2f m", Clamp);
  bool faceMe = panel.mFaceOrigin;
  if (ImGui::Checkbox("Turn to face me", &faceMe)) {
    if (!faceMe) {
      // Start from the way it faces now, so it doesn't jump
      panel.mYawDegrees
        = std::atan2(-panel.mPosition[0], -panel.mPosition[2]) * 180.0f / std::numbers::pi_v<float>;
    }
    panel.mFaceOrigin = faceMe;
    changed = true;
  }
  if (!panel.mFaceOrigin) {
    changed |= ImGui::SliderFloat("Turn", &panel.mYawDegrees, -180.0f, 180.0f, "%.0f\xC2\xB0", Clamp);
    Help("Positive turns it to its left.");
  }

  if (isWindow) {
    ImGui::SeparatorText("Trim the edges");
    float percent[4] {panel.mCrop.mLeft * 100, panel.mCrop.mTop * 100, panel.mCrop.mRight * 100, panel.mCrop.mBottom * 100};
    constexpr const char* edges[4] {"Left edge", "Top edge", "Right edge", "Bottom edge"};
    bool cropped = false;
    for (int i = 0; i < 4; ++i) {
      cropped |= ImGui::SliderFloat(edges[i], &percent[i], 0.0f, 45.0f, "%.0f%%", Clamp);
      if (i == 0) {
        Help("For title bars and toolbars the program draws inside its window. Windows' own frame is already trimmed.");
      }
    }
    if (cropped) {
      panel.mCrop = Render::Crop {percent[0] / 100, percent[1] / 100, percent[2] / 100, percent[3] / 100}.Clamped();
      changed = true;
    }
  }

  ImGui::SeparatorText("Updates");
  int rate = static_cast<int>(std::lround(panel.mMaxUpdatesPerSecond));
  const auto fastest = std::format("Fastest ({:.0f} a second)", Panels::DefaultMaxUpdatesPerSecond);
  const int maxRate = static_cast<int>(Panels::DefaultMaxUpdatesPerSecond);
  if (ImGui::SliderInt("Most updates a second", &rate, 0, maxRate, rate == 0 ? fastest.c_str() : "%d a second", Clamp)) {
    panel.mMaxUpdatesPerSecond = rate;
    changed = true;
  }
  Help("Lower is lighter on your PC. Panels only update when what they show changes.");
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

void SettingsWindow::WindowPicker() {
  constexpr const char* Title = "Pick a window";
  if (mOpenPicker) {
    mOpenPicker = false;
    ImGui::OpenPopup(Title);
    mFilter.fill(0);
    mWindows.clear();
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
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      constexpr auto rowFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
      if (ImGui::Selectable(Utf8(window.mTitle).c_str(), i == mPickerSelection, rowFlags)) {
        mPickerSelection = i;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          chosen = i;
        }
      }
      ImGui::PopID();
      ImGui::TableNextColumn();
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

}// namespace OpenDesktopVR::Host
