// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// A window to capture as a panel: it logs every click it gets,
// to check that finger taps arrive as real clicks.
//
// Usage: click-target-test <log file> [slow ms]
// Each line of the log is "DOWN x y t", "UP x y t", "WHEEL delta x y", or
// "MOVES n" (a count of mouse moves since the last button event); t is a
// high-resolution timestamp in milliseconds, comparable between processes.
//
// 'slow ms' makes every mouse move take that long to handle, like a busy app.

#include <Windows.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string>

namespace {

std::ofstream gLog;
int gMoves = 0;
POINT gLastClick {-1, -1};
int gClickCount = 0;
int gSlowMilliseconds = 0;

double NowMilliseconds() {
  static const auto frequency = [] {
    LARGE_INTEGER f {};
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER now {};
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) * 1000.0 / frequency;
}

void BusyWait(int milliseconds) {
  const auto until = NowMilliseconds() + milliseconds;
  while (NowMilliseconds() < until) {
  }
}

void Log(const std::string& line) {
  if (gMoves > 0) {
    const auto moves = std::format("MOVES {}", gMoves);
    gLog << moves << std::endl;
    std::printf("%s\n", moves.c_str());
    gMoves = 0;
  }
  gLog << line << std::endl;
  std::printf("%s\n", line.c_str());
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEMOVE:
      ++gMoves;
      if (gSlowMilliseconds > 0) {
        BusyWait(gSlowMilliseconds);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      const auto now = NowMilliseconds();
      gLastClick = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ++gClickCount;
      Log(std::format("DOWN {} {} {:.3f}", gLastClick.x, gLastClick.y, now));
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_LBUTTONUP:
      Log(std::format(
        "UP {} {} {:.3f}",
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam),
        NowMilliseconds()));
      return 0;
    case WM_MOUSEWHEEL: {
      // Screen coordinates
      POINT point {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ScreenToClient(hwnd, &point);
      Log(std::format(
        "WHEEL {} {} {}",
        GET_WHEEL_DELTA_WPARAM(wParam),
        point.x,
        point.y));
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      const auto dc = BeginPaint(hwnd, &ps);
      RECT client;
      GetClientRect(hwnd, &client);
      // A grid, so the captured window has something to show
      const auto background = CreateSolidBrush(RGB(30, 34, 44));
      FillRect(dc, &client, background);
      DeleteObject(background);
      const auto pen = CreatePen(PS_SOLID, 1, RGB(70, 80, 100));
      SelectObject(dc, pen);
      for (int x = 0; x < client.right; x += 50) {
        MoveToEx(dc, x, 0, nullptr);
        LineTo(dc, x, client.bottom);
      }
      for (int y = 0; y < client.bottom; y += 50) {
        MoveToEx(dc, 0, y, nullptr);
        LineTo(dc, client.right, y);
      }
      DeleteObject(pen);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(220, 225, 235));
      const auto text = std::format(
        L"Click target - {} clicks - last at ({}, {})",
        gClickCount,
        gLastClick.x,
        gLastClick.y);
      TextOutW(dc, 20, 20, text.c_str(), static_cast<int>(text.size()));
      if (gLastClick.x >= 0) {
        const auto marker = CreateSolidBrush(RGB(255, 120, 60));
        const RECT r {
          gLastClick.x - 8, gLastClick.y - 8, gLastClick.x + 8, gLastClick.y + 8};
        FillRect(dc, &r, marker);
        DeleteObject(marker);
      }
      EndPaint(hwnd, &ps);
      // Flush GDI and the compositor, like a real app presenting a frame
      GdiFlush();
      if (gLastClick.x >= 0) {
        Log(std::format("PAINT {:.3f}", NowMilliseconds()));
      }
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}// namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("Usage: click-target-test <log file>\n");
    return 1;
  }
  gLog.open(argv[1], std::ios::out | std::ios::trunc);
  if (argc > 2) {
    gSlowMilliseconds = std::atoi(argv[2]);
  }

  const auto instance = GetModuleHandleW(nullptr);
  const WNDCLASSW windowClass {
    .lpfnWndProc = &WindowProc,
    .hInstance = instance,
    .hCursor = LoadCursorW(nullptr, IDC_ARROW),
    .lpszClassName = L"OpenDesktopVR.ClickTarget",
  };
  RegisterClassW(&windowClass);

  RECT rect {0, 0, 800, 600};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  const auto hwnd = CreateWindowExW(
    0,
    windowClass.lpszClassName,
    L"Click Target",
    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
    100,
    100,
    rect.right - rect.left,
    rect.bottom - rect.top,
    nullptr,
    nullptr,
    instance,
    nullptr);
  if (!hwnd) {
    std::printf("Failed to create window\n");
    return 1;
  }
  Log("READY");

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
