// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// A window to capture as a panel: it logs every click it gets,
// to check that finger taps arrive as real clicks.
//
// Usage: click-target-test <log file> [slow ms] [animate fps] [same]
//          [--hang-once <ms>]
// Each line of the log is "DOWN x y t", "UP x y t", "WHEEL delta x y", or
// "MOVES n" (a count of mouse moves since the last button event); t is a
// high-resolution timestamp in milliseconds, comparable between processes.
// Added in the port (M4b): "DOUBLE x y t" for a double-click (the window
// asks for them), "LEAVE t" when the mouse leaves it; '--hang-once <ms>'
// stops responding for that long on the first click, like a hung app.
//
// 'slow ms' makes every mouse move take that long to handle, like a busy app.
// 'animate fps' repaints that many times a second with a moving bar, like a
// playing video, evenly paced on the display's refresh (DwmFlush); the
// title becomes "Click Target (animated)". With 'same', it repaints that
// often without changing anything, like an idle window on Windows 10.
// "Click Target (repainting)".

#include <Windows.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::ofstream gLog;
int gMoves = 0;
POINT gLastClick {-1, -1};
int gClickCount = 0;
int gSlowMilliseconds = 0;
int gAnimateFps = 0;
bool gSame = false;
int gHangOnceMilliseconds = 0;
std::atomic<uint64_t> gAnimationFrame {0};

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
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      const auto now = NowMilliseconds();
      gLastClick = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ++gClickCount;
      Log(std::format("{} {} {} {:.3f}", msg == WM_LBUTTONDBLCLK ? "DOUBLE" : "DOWN", gLastClick.x, gLastClick.y, now));
      InvalidateRect(hwnd, nullptr, TRUE);
      if (gHangOnceMilliseconds > 0) {
        Sleep(static_cast<DWORD>(std::exchange(gHangOnceMilliseconds, 0)));
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      Log(std::format("LEAVE {:.3f}", NowMilliseconds()));
      return 0;
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
      if (gAnimateFps > 0 && !gSame) {
        // A bar that moves every frame: each frame differs from the last
        const auto bar = CreateSolidBrush(RGB(90, 200, 120));
        const LONG x = static_cast<LONG>((gAnimationFrame.load() * 7) % static_cast<uint64_t>(std::max<LONG>(client.right - 40, 1)));
        const RECT r {x, 60, x + 40, client.bottom - 20};
        FillRect(dc, &r, bar);
        DeleteObject(bar);
      }
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
  // Named options can go anywhere; what's left is positional
  std::vector<char*> positional;
  for (int i = 0; i < argc; ++i) {
    if (std::string_view {argv[i]} == "--hang-once" && i + 1 < argc) {
      gHangOnceMilliseconds = std::max(0, std::atoi(argv[++i]));
    } else {
      positional.push_back(argv[i]);
    }
  }
  argc = static_cast<int>(positional.size());
  argv = positional.data();
  gLog.open(argv[1], std::ios::out | std::ios::trunc);
  if (argc > 2) {
    gSlowMilliseconds = std::atoi(argv[2]);
  }
  if (argc > 3) {
    gAnimateFps = std::max(0, std::min(240, std::atoi(argv[3])));
  }
  gSame = argc > 4 && std::string {argv[4]} == "same";

  const auto instance = GetModuleHandleW(nullptr);
  const WNDCLASSW windowClass {
    .style = CS_DBLCLKS,
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
    gAnimateFps == 0 ? L"Click Target" : gSame ? L"Click Target (repainting)" : L"Click Target (animated)",
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
  if (gAnimateFps > 0) {
    // Paced on the display's refresh, like a video player: a new frame on
    // the first refresh at least one period after the last, so frames are
    // evenly spaced at the source (a free-running timer drifts against the
    // refresh and stutters by itself)
    std::thread([hwnd] {
      const double period = 1000.0 / gAnimateFps;
      double last = NowMilliseconds() - period;
      while (IsWindow(hwnd)) {
        if (FAILED(DwmFlush())) {
          Sleep(1);
        }
        const double now = NowMilliseconds();
        if (now - last >= period - 4.0) {
          last = now;
          ++gAnimationFrame;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
    }).detach();
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
