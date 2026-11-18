// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The injection matrix's Win32 target (doc 11, M5a): a window filled by a
// multiline EDIT control, as a plain Win32 app would have. key-inject-test
// types into it and reads its text and selection back (WM_GETTEXT,
// EM_GETSEL).
//
//   edit-target-app [<log file>]
//
// The window's class is OdvrEditTarget; the EDIT's control ID is 1. With a
// log file, the EDIT's text is written to it (UTF-8) whenever it changes,
// for tests that can't read the window (the simulator's typing, M5b).

#include <Windows.h>

#include <string>

namespace {

HWND gEdit {nullptr};
std::wstring gLog;

void WriteLog() {
  if (gLog.empty()) {
    return;
  }
  const int length = GetWindowTextLengthW(gEdit);
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  text.resize(static_cast<std::size_t>(GetWindowTextW(gEdit, text.data(), length + 1)));
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
  const HANDLE file = CreateFileW(gLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
  }
}

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
    case WM_COMMAND:
      if (HIWORD(wParam) == EN_CHANGE) {
        WriteLog();
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

}// namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  if (commandLine && *commandLine) {
    gLog = commandLine;
    if (gLog.size() >= 2 && gLog.front() == L'"' && gLog.back() == L'"') {
      gLog = gLog.substr(1, gLog.size() - 2);
    }
  }
  WNDCLASSW windowClass {};
  windowClass.lpfnWndProc = &WindowProc;
  windowClass.hInstance = instance;
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = L"OdvrEditTarget";
  RegisterClassW(&windowClass);
  CreateWindowExW(0, L"OdvrEditTarget", L"edit-target", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 700, 500, nullptr,
    nullptr, instance, nullptr);
  WriteLog();// empty, so a reader knows it started
  MSG message {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
