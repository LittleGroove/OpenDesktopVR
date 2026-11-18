// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <dwmapi.h>

#include <algorithm>

namespace OpenDesktopVR::WindowUtil {

namespace {

std::wstring ProcessPath(DWORD processID) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processID);
  if (!process) {
    return {};
  }
  std::wstring path(MAX_PATH * 4, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
  CloseHandle(process);
  if (!ok) {
    return {};
  }
  path.resize(size);
  return path;
}

}// namespace

bool IsCloaked(HWND window) noexcept {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

std::optional<WindowInfo> Describe(HWND window) noexcept {
  try {
    if (!IsWindow(window)) {
      return std::nullopt;
    }
    WindowInfo info;
    info.mWindow = window;
    const int length = GetWindowTextLengthW(window);
    if (length > 0) {
      info.mTitle.resize(static_cast<std::size_t>(length) + 1);
      info.mTitle.resize(static_cast<std::size_t>(GetWindowTextW(window, info.mTitle.data(), length + 1)));
    }
    wchar_t className[256] {};
    info.mClassName.assign(className, static_cast<std::size_t>(GetClassNameW(window, className, 256)));
    GetWindowThreadProcessId(window, &info.mProcessID);
    info.mExecutablePath = ProcessPath(info.mProcessID);
    return info;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<Thumbnail> GrabThumbnail(HWND window, uint32_t maxWidth, uint32_t maxHeight) noexcept {
#ifndef PW_RENDERFULLCONTENT
  constexpr UINT PW_RENDERFULLCONTENT = 0x00000002;// Windows 8.1 and later
#endif
  if (!window || !IsWindow(window) || IsIconic(window) || IsHungAppWindow(window) || maxWidth == 0 || maxHeight == 0) {
    return std::nullopt;
  }
  RECT bounds {};
  if (!GetWindowRect(window, &bounds)) {
    return std::nullopt;
  }
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
    return std::nullopt;
  }
  const double scale = std::min({1.0, static_cast<double>(maxWidth) / width, static_cast<double>(maxHeight) / height});
  const int smallWidth = std::max(1, static_cast<int>(width * scale));
  const int smallHeight = std::max(1, static_cast<int>(height * scale));

  std::optional<Thumbnail> result;
  HDC screen = GetDC(nullptr);
  HDC full = CreateCompatibleDC(screen);
  HBITMAP fullBitmap = CreateCompatibleBitmap(screen, width, height);
  HDC smallDC = CreateCompatibleDC(screen);
  BITMAPINFO info {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = smallWidth;
  info.bmiHeader.biHeight = -smallHeight;// top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP smallBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (full && fullBitmap && smallDC && smallBitmap && bits) {
    const auto oldFull = SelectObject(full, fullBitmap);
    const auto oldSmall = SelectObject(smallDC, smallBitmap);
    if (PrintWindow(window, full, PW_RENDERFULLCONTENT)) {
      SetStretchBltMode(smallDC, HALFTONE);
      SetBrushOrgEx(smallDC, 0, 0, nullptr);
      StretchBlt(smallDC, 0, 0, smallWidth, smallHeight, full, 0, 0, width, height, SRCCOPY);
      GdiFlush();
      Thumbnail thumbnail {static_cast<uint32_t>(smallWidth), static_cast<uint32_t>(smallHeight), {}};
      const auto* source = static_cast<const uint8_t*>(bits);
      thumbnail.mBgra.assign(source, source + (static_cast<std::size_t>(smallWidth) * smallHeight * 4));
      for (std::size_t i = 3; i < thumbnail.mBgra.size(); i += 4) {
        thumbnail.mBgra[i] = 255;// GDI leaves alpha undefined
      }
      result = std::move(thumbnail);
    }
    SelectObject(full, oldFull);
    SelectObject(smallDC, oldSmall);
  }
  if (smallBitmap) {
    DeleteObject(smallBitmap);
  }
  if (smallDC) {
    DeleteDC(smallDC);
  }
  if (fullBitmap) {
    DeleteObject(fullBitmap);
  }
  if (full) {
    DeleteDC(full);
  }
  ReleaseDC(nullptr, screen);
  return result;
}

std::vector<WindowInfo> ListCaptureCandidates() noexcept {
  std::vector<WindowInfo> windows;
  try {
    EnumWindows(
      [](HWND window, LPARAM context) -> BOOL {
        auto& list = *reinterpret_cast<std::vector<WindowInfo>*>(context);
        if (!IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window || IsCloaked(window)) {
          return TRUE;
        }
        const auto exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) {
          return TRUE;
        }
        DWORD processID = 0;
        GetWindowThreadProcessId(window, &processID);
        if (processID == GetCurrentProcessId()) {
          return TRUE;// never our own windows
        }
        if (auto info = Describe(window); info && !info->mTitle.empty()) {
          list.push_back(std::move(*info));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windows));
    std::ranges::sort(windows, [](const WindowInfo& a, const WindowInfo& b) { return a.mTitle < b.mTitle; });
  } catch (...) {
  }
  return windows;
}

std::optional<RECT> ClientCropInFrame(HWND window, uint32_t frameWidth, uint32_t frameHeight) noexcept {
  RECT frame {};
  if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
    return std::nullopt;
  }
  RECT client {};
  if (!GetClientRect(window, &client)) {
    return std::nullopt;
  }
  POINT topLeft {client.left, client.top};
  POINT bottomRight {client.right, client.bottom};
  if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) {
    return std::nullopt;
  }
  RECT crop {
    topLeft.x - frame.left,
    topLeft.y - frame.top,
    bottomRight.x - frame.left,
    bottomRight.y - frame.top,
  };
  crop.left = std::clamp<LONG>(crop.left, 0, static_cast<LONG>(frameWidth));
  crop.top = std::clamp<LONG>(crop.top, 0, static_cast<LONG>(frameHeight));
  crop.right = std::clamp<LONG>(crop.right, crop.left, static_cast<LONG>(frameWidth));
  crop.bottom = std::clamp<LONG>(crop.bottom, crop.top, static_cast<LONG>(frameHeight));
  if (crop.right - crop.left < 1 || crop.bottom - crop.top < 1) {
    return std::nullopt;
  }
  return crop;
}

std::wstring ExecutableName(const WindowInfo& info) {
  return std::wstring {StringUtil::FileNamePart(info.mExecutablePath)};
}

}// namespace OpenDesktopVR::WindowUtil
