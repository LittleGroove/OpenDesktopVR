// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "TrayIcon.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Product.hpp>

#include <shellapi.h>

#include <format>
#include <string>

namespace OpenDesktopVR::Host {

namespace {

constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT_PTR QuitTimer = 1;
constexpr UINT TrayID = 1;

enum MenuCommand : UINT {
  ToggleVisible = 100,
  OpenLogs,
  Exit,
};

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.Host";

}// namespace

TrayIcon::TrayIcon(HINSTANCE instance, RenderLoop& renderer) : mInstance(instance), mRenderer(renderer) {
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &TrayIcon::WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  // A hidden top-level window: receives the tray icon's messages
  mWindow = CreateWindowExW(0, WindowClass, Product::DisplayName.data(), 0, 0, 0, 0, 0, nullptr, nullptr, instance, this);
  // Explorer restarted: put the icon back
  mTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  AddIcon();
}

TrayIcon::~TrayIcon() {
  if (mIconAdded) {
    NOTIFYICONDATAW data {sizeof(data)};
    data.hWnd = mWindow;
    data.uID = TrayID;
    Shell_NotifyIconW(NIM_DELETE, &data);
  }
  if (mWindow) {
    DestroyWindow(mWindow);
  }
  UnregisterClassW(WindowClass, mInstance);
}

void TrayIcon::AddIcon() {
  NOTIFYICONDATAW data {sizeof(data)};
  data.hWnd = mWindow;
  data.uID = TrayID;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = TrayMessage;
  data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  const auto tip = std::format(L"{}: {}", Product::DisplayName, Product::Tagline);
  wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
  mIconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
}

void TrayIcon::QuitAfter(double seconds) {
  SetTimer(mWindow, QuitTimer, static_cast<UINT>(seconds * 1000), nullptr);
}

int TrayIcon::Run() {
  MSG message {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

void TrayIcon::ShowMenu() {
  HMENU menu = CreatePopupMenu();
  const auto title = std::format(L"{}: {}", Product::DisplayName, Product::Tagline);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ToggleVisible, mRenderer.PanelsVisible() ? L"Hide panels" : L"Show panels");
  AppendMenuW(menu, MF_STRING, OpenLogs, L"Open logs folder");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, Exit, L"Exit");
  POINT cursor {};
  GetCursorPos(&cursor);
  // Required for the menu to close when clicking elsewhere
  SetForegroundWindow(mWindow);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, mWindow, nullptr);
  DestroyMenu(menu);
}

LRESULT CALLBACK TrayIcon::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self && self->mWindow == window) {
    return self->Handle(message, wParam, lParam);
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TrayIcon::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == mTaskbarCreated && mTaskbarCreated != 0) {
    AddIcon();
    return 0;
  }
  switch (message) {
    case TrayMessage:
      if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
        ShowMenu();
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case ToggleVisible:
          mRenderer.SetPanelsVisible(!mRenderer.PanelsVisible());
          Log::Info("Panels {}", mRenderer.PanelsVisible() ? "shown" : "hidden");
          return 0;
        case OpenLogs: {
          const auto folder = Product::LogsFolder();
          ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          return 0;
        }
        case Exit:
          PostQuitMessage(0);
          return 0;
      }
      break;
    case WM_TIMER:
      if (wParam == QuitTimer) {
        PostQuitMessage(0);
        return 0;
      }
      break;
    case WM_ENDSESSION:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(mWindow, message, wParam, lParam);
}

}// namespace OpenDesktopVR::Host
