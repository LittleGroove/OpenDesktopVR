// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include "TrayIcon.hpp"

#include <OpenDesktopVR/CrashHandler.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Product.hpp>

#include <shellapi.h>

#include <format>
#include <string>

namespace OpenDesktopVR::Host {

namespace {

constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT_PTR QuitTimer = 1;
constexpr UINT_PTR ListingTimer = 2;// polls the worker listing windows for the menu
constexpr UINT_PTR GameTimer = 3;// the attached game, for per-game profiles (ADR-038)
constexpr UINT ListingPollMs = 15;
constexpr UINT TrayID = 1;

enum MenuCommand : UINT {
  ToggleVisible = 100,
  OpenSettings,
  OpenLogs,
  Exit,
  AddWindowFirst = 1000,// + index into the listed windows
  RemovePanelFirst = 2000,// + index into the listed panels
  ProfileFirst = 3000,// + index into the listed profiles
};
constexpr UINT MaxListed = 40;

std::wstring MenuText(std::wstring text) {
  // '&' marks a menu accelerator; show it literally
  for (std::size_t at = text.find(L'&'); at != std::wstring::npos; at = text.find(L'&', at + 2)) {
    text.insert(at, 1, L'&');
  }
  if (text.size() > 60) {
    text = text.substr(0, 57) + L"...";
  }
  return text;
}

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.Host";

}// namespace

TrayIcon::TrayIcon(HINSTANCE instance, RenderLoop& renderer, PanelManager& panels, SettingsWindow& settings)
  : mInstance(instance), mRenderer(renderer), mPanels(panels), mSettings(settings) {
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &CrashHandler::GuardedProcedure<&TrayIcon::WindowProc>;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  // A hidden top-level window: receives the tray icon's messages
  mWindow = CreateWindowExW(0, WindowClass, Product::DisplayName.data(), 0, 0, 0, 0, 0, nullptr, nullptr, instance, this);
  // Explorer restarted: put the icon back
  mTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  AddIcon();
  SetTimer(mWindow, GameTimer, 1000, nullptr);
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

void TrayIcon::ShowWarning(std::wstring_view title, std::wstring_view text) {
  if (!mIconAdded) {
    Log::Warning("Tray: no icon (Explorer isn't running?), so this warning isn't shown");
    return;
  }
  NOTIFYICONDATAW data {sizeof(data)};
  data.hWnd = mWindow;
  data.uID = TrayID;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_WARNING;
  wcsncpy_s(data.szInfoTitle, std::wstring {title}.c_str(), _TRUNCATE);
  wcsncpy_s(data.szInfo, std::wstring {text}.c_str(), _TRUNCATE);
  const bool shown = Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
  Log::Info("Tray: warning {}", shown ? "shown" : "not shown (notifications refused it)");
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

void TrayIcon::RequestMenu() {
  if (mListing.Started()) {
    return;// already on its way
  }
  try {
    mListing = ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>>::Start(
      L"Window list", [] { return WindowUtil::ListCaptureCandidates(); });
  } catch (...) {
    ThreadUtil::ReportEscapedException("listing windows");
    ShowMenu({});
    return;
  }
  SetTimer(mWindow, ListingTimer, ListingPollMs, nullptr);
}

void TrayIcon::ShowMenu(std::vector<WindowUtil::WindowInfo> listed) {
  HMENU menu = CreatePopupMenu();
  const auto title = std::format(L"{}: {}", Product::DisplayName, Product::Tagline);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, OpenSettings, L"Settings...");
  SetMenuDefaultItem(menu, OpenSettings, FALSE);// what a left-click does
  // Show a window in VR: every window that could be captured a moment ago
  HMENU windows = CreatePopupMenu();
  mMenuWindows = std::move(listed);
  if (mMenuWindows.size() > MaxListed) {
    mMenuWindows.resize(MaxListed);
  }
  for (UINT i = 0; i < mMenuWindows.size(); ++i) {
    const auto& window = mMenuWindows[i];
    const auto text = MenuText(std::format(L"{}   ({})", window.mTitle, WindowUtil::ExecutableName(window)));
    AppendMenuW(windows, MF_STRING, AddWindowFirst + i, text.c_str());
  }
  if (mMenuWindows.empty()) {
    AppendMenuW(windows, MF_STRING | MF_GRAYED, 0, L"No windows to show");
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(windows), L"Show a window in VR");

  HMENU remove = CreatePopupMenu();
  mMenuPanels.clear();
  for (const auto& panel: mPanels.Panels()) {
    if (mMenuPanels.size() >= MaxListed) {
      break;
    }
    AppendMenuW(remove, MF_STRING, RemovePanelFirst + static_cast<UINT>(mMenuPanels.size()), MenuText(panel.mName).c_str());
    mMenuPanels.push_back(panel.mID);
  }
  if (mMenuPanels.empty()) {
    AppendMenuW(remove, MF_STRING | MF_GRAYED, 0, L"No panels");
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(remove), L"Remove a panel");

  // Profiles (ADR-038): the active one ticked
  HMENU profiles = CreatePopupMenu();
  mMenuProfiles.clear();
  for (const auto& profile: mPanels.Profiles()) {
    if (mMenuProfiles.size() >= MaxListed) {
      break;
    }
    const UINT flags = MF_STRING | (profile.mID == mPanels.ActiveProfile() ? MF_CHECKED : 0);
    AppendMenuW(profiles, flags, ProfileFirst + static_cast<UINT>(mMenuProfiles.size()), MenuText(profile.mName).c_str());
    mMenuProfiles.push_back(profile.mID);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profiles), L"Profile");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ToggleVisible, mPanels.AnyShowing() ? L"Hide panels" : L"Show panels");
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
    // Nothing may unwind through Windows (TH-8)
    LRESULT result = 0;
    if (ThreadUtil::CatchAll("tray icon", [&] { result = self->Handle(message, wParam, lParam); })) {
      return result;
    }
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
      if (LOWORD(lParam) == WM_LBUTTONUP) {
        mSettings.Show();
      } else if (LOWORD(lParam) == WM_RBUTTONUP) {
        RequestMenu();
      } else if (LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
        // The only notification is ShowWarning's, explained there
        mSettings.ShowDiagnosticsTab();
        mSettings.Show();
      }
      return 0;
    case WM_COMMAND: {
      const UINT command = LOWORD(wParam);
      if (command >= AddWindowFirst && command < AddWindowFirst + mMenuWindows.size()) {
        mPanels.AddWindow(mMenuWindows[command - AddWindowFirst]);
        mSettings.Invalidate();
        return 0;
      }
      if (command >= RemovePanelFirst && command < RemovePanelFirst + mMenuPanels.size()) {
        mPanels.Remove(mMenuPanels[command - RemovePanelFirst]);
        mSettings.Invalidate();
        return 0;
      }
      if (command >= ProfileFirst && command < ProfileFirst + mMenuProfiles.size()) {
        mPanels.SwitchProfile(mMenuProfiles[command - ProfileFirst], "tray menu");
        mSettings.Invalidate();
        return 0;
      }
      switch (command) {
        case OpenSettings:
          mSettings.Show();
          return 0;
        case ToggleVisible:
          Log::Info("Panels {} (tray menu)", mPanels.ToggleAll() ? "shown" : "hidden");
          mSettings.Invalidate();
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
    }
    case WM_TIMER:
      if (wParam == QuitTimer) {
        PostQuitMessage(0);
        return 0;
      }
      if (wParam == GameTimer) {
        const auto stats = mRenderer.Stats();
        mPanels.FollowGame(stats.mLayerProcessID, stats.mLayerExecutable);
        return 0;
      }
      if (wParam == ListingTimer) {
        if (!mListing.Running()) {
          KillTimer(mWindow, ListingTimer);
          ShowMenu(mListing.Take().value_or(std::vector<WindowUtil::WindowInfo> {}));
        }
        return 0;
      }
      break;
    case WM_QUERYENDSESSION:
      // Windows signing out or shutting down, or the setup program closing
      // it through the Restart Manager (M7a): nothing to ask, so yes
      return TRUE;
    case WM_ENDSESSION:
      // wParam false: another program cancelled the sign-out, so it stays
      if (wParam) {
        Log::Info("Closing: {}", (lParam & ENDSESSION_CLOSEAPP) ? "the setup program asked (the Restart Manager)"
                                                                  : "Windows is signing out or shutting down");
        // Saved now: the process may be ended before the message loop runs again
        mPanels.FlushSettings();
        PostQuitMessage(0);
      }
      return 0;
  }
  return DefWindowProcW(mWindow, message, wParam, lParam);
}

}// namespace OpenDesktopVR::Host
