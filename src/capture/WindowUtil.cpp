// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

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
