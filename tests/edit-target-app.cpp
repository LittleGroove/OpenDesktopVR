// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The injection matrix's Win32 target (doc 11, M5a): a window filled by a
// multiline EDIT control, as a plain Win32 app would have. key-inject-test
// types into it and reads its text and selection back (WM_GETTEXT,
// EM_GETSEL).
//
//   edit-target-app
//
// The window's class is OdvrEditTarget; the EDIT's control ID is 1.

#include <Windows.h>

namespace {

HWND gEdit {nullptr};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      gEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
      SendMessageW(gEdit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
      return 0;
    case WM_SIZE:
      MoveWindow(gEdit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
      return 0;
    case WM_SETFOCUS:
      // As Notepad does: the window's focus is its EDIT's
      SetFocus(gEdit);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

}// namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  WNDCLASSW windowClass {};
  windowClass.lpfnWndProc = &WindowProc;
  windowClass.hInstance = instance;
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = L"OdvrEditTarget";
  RegisterClassW(&windowClass);
  CreateWindowExW(0, L"OdvrEditTarget", L"edit-target", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 700, 500, nullptr,
    nullptr, instance, nullptr);
  MSG message {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
