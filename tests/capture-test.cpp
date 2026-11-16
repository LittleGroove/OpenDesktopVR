// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Window capture (doc 04; CP-1, CP-3, CP-5, CP-6, CP-7): match rules, and a
// real window (the click-target tool, in its own process) captured into a
// panel texture: client area only, resize, frames dropped while inactive,
// window closed and reopened.
//
//   capture-test <click-target exe>

#include "check.hpp"

#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/PanelRing.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>
#include <OpenDesktopVR/WindowCaptureSource.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <winrt/base.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <functional>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;
using namespace std::chrono_literals;

namespace {

std::wstring gTargetExe;

struct Target {
  PROCESS_INFORMATION mInfo {};
  Target() {
    // The click target logs clicks to a file it's given
    const auto log = (std::filesystem::temp_directory_path() / std::format(L"odvr-capture-test-{}.log", GetCurrentProcessId())).wstring();
    std::wstring command = L"\"" + gTargetExe + L"\" \"" + log + L"\"";
    STARTUPINFOW startup {sizeof(startup)};
    CreateProcessW(gTargetExe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &mInfo);
    WaitForInputIdle(mInfo.hProcess, 5000);
  }
  HWND Window() const {
    for (const auto& window: WindowUtil::ListCaptureCandidates()) {
      if (window.mProcessID == mInfo.dwProcessId) {
        return window.mWindow;
      }
    }
    return nullptr;
  }
  void Kill() {
    if (mInfo.hProcess) {
      TerminateProcess(mInfo.hProcess, 0);
      WaitForSingleObject(mInfo.hProcess, 5000);
      CloseHandle(mInfo.hProcess);
      CloseHandle(mInfo.hThread);
      mInfo = {};
    }
  }
  ~Target() {
    Kill();
  }
};

bool WaitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit, const std::function<void()>& tick = {}) {
  const auto end = TimeUtil::Now() + limit;
  while (TimeUtil::Now() < end) {
    if (tick) {
      tick();
    }
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return condition();
}

// Renders the source into a ring slot and reads one pixel back (BGRA)
uint32_t RenderAndSample(Render::GpuDevice& gpu, Capture::WindowCaptureSource& source, uint32_t x, uint32_t y) {
  // Take what arrived, as the render loop does: the GPU check answers a
  // frame later
  WaitFor([&] { return source.Prepare() != Render::Readiness::Pending; }, 2s);
  const auto size = source.Size();
  auto ring = Render::PanelRing::Create(gpu, size.mWidth, size.mHeight, DXGI_FORMAT_B8G8R8A8_UNORM);
  if (!ring) {
    return 0;
  }
  auto* d2d = gpu.D2D();
  d2d->SetTarget(ring->Target(0));
  d2d->BeginDraw();
  source.Render(d2d, size, TimeUtil::Now());
  d2d->EndDraw();
  d2d->SetTarget(nullptr);
  winrt::com_ptr<IDXGISurface> surface;
  ring->Target(0)->GetSurface(surface.put());
  winrt::com_ptr<ID3D11Texture2D> texture;
  surface->QueryInterface(texture.put());
  D3D11_TEXTURE2D_DESC desc {};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  winrt::com_ptr<ID3D11Texture2D> staging;
  gpu.Device()->CreateTexture2D(&desc, nullptr, staging.put());
  gpu.Context()->CopyResource(staging.get(), texture.get());
  D3D11_MAPPED_SUBRESOURCE mapped {};
  if (FAILED(gpu.Context()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return 0;
  }
  const auto pixel = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4);
  gpu.Context()->Unmap(staging.get(), 0);
  return pixel;
}

}// namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("Usage: capture-test <click-target exe>\n");
    return 2;
  }
  gTargetExe = std::filesystem::absolute(argv[1]).wstring();
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  Scenario("Match rules: executable, class and title, ignoring case", [] {
    WindowUtil::WindowInfo window {nullptr, L"Discord - #general", L"Chrome_WidgetWin_1", L"C:\\Apps\\Discord\\Discord.exe", 1};
    WindowMatch::Rule rule {L"c:\\apps\\discord\\discord.EXE", L"chrome_widgetwin_1", L"discord", WindowMatch::TitleMode::Prefix};
    Check(WindowMatch::Matches(rule, window), "prefix, any case");
    rule.mTitleMode = WindowMatch::TitleMode::Exact;
    Check(!WindowMatch::Matches(rule, window), "exact doesn't match a longer title");
    rule.mTitle = L"#GENERAL";
    rule.mTitleMode = WindowMatch::TitleMode::Contains;
    Check(WindowMatch::Matches(rule, window), "contains");
    rule.mTitle = L"^discord - #\\w+$";
    rule.mTitleMode = WindowMatch::TitleMode::Regex;
    Check(WindowMatch::Matches(rule, window), "regex");
    rule.mTitle = L"([unclosed";
    Check(!WindowMatch::Matches(rule, window), "a malformed regex never matches, never throws");
    rule = {L"C:\\Other.exe", L"", L"", WindowMatch::TitleMode::Contains};
    Check(!WindowMatch::Matches(rule, window), "another executable doesn't match");
    // New rules: program and class must match; the title is only a preference
    auto preferred = WindowMatch::FromWindow(window);
    Check(preferred.mTitleMode == WindowMatch::TitleMode::Prefer, "new rules prefer the title, never require it");
    WindowUtil::WindowInfo renamed = window;
    renamed.mTitle = L"Discord - #another-channel";
    Check(WindowMatch::Matches(preferred, renamed), "a changed title still matches (a new channel, song or tab)");
    WindowUtil::WindowInfo otherApp = window;
    otherApp.mExecutablePath = LR"(C:\Apps\Other.exe)";
    Check(!WindowMatch::Matches(preferred, otherApp), "another program still doesn't");
    const auto json = WindowMatch::ToJson(WindowMatch::FromWindow(window));
    Check(WindowMatch::FromJson(json) == WindowMatch::FromWindow(window), "round-trips through settings JSON");
    Check(!WindowMatch::FromJson(JsonUtil::Json::object()), "an empty rule (matching everything) is refused");
  });

  Check(Capture::IsCaptureSupported(), "Windows.Graphics.Capture is supported here");
  auto gpu = Render::GpuDevice::Create({.mAllowWarp = true});
  Check(gpu != nullptr, "a GPU device");
  if (!gpu) {
    return Finish();
  }

  Target target;
  HWND window = nullptr;
  WaitFor([&] { return (window = target.Window()) != nullptr; }, 5s);
  Check(window != nullptr, "the click-target window is open");
  if (!window) {
    return Finish();
  }
  const auto info = WindowUtil::Describe(window);
  std::atomic<int> dirty {0};
  Capture::WindowCaptureSource source {*gpu, {WindowMatch::FromWindow(*info), L"Click target"}, [&] { ++dirty; }};

  Scenario("A window is found and captured, client area only (CP-3)", [&] {
    Check(WaitFor([&] { return source.FramesArrived() > 0; }, 5s, [&] { source.Maintain(TimeUtil::Now()); }), "frames arrive");
    Check(dirty > 0, "a new frame marks the panel dirty (CP-2)");
    // Take the frame, as the render loop does, before asking its size
    WaitFor([&] { return source.Prepare() != Render::Readiness::Pending; }, 2s);
    RECT client {};
    GetClientRect(window, &client);
    const auto size = source.Size();
    Check(size.mWidth == static_cast<uint32_t>(client.right) && size.mHeight == static_cast<uint32_t>(client.bottom),
      std::format("panel size is the client area: {}x{} (client {}x{})", size.mWidth, size.mHeight, client.right, client.bottom));
    // The click target paints its client area RGB(30, 34, 44)
    // The first frame can arrive before the window has painted (transparent
    // pixels): give it up to 2 s to paint
    uint32_t pixel = 0;
    WaitFor([&] {
      pixel = RenderAndSample(*gpu, source, 2, 2);
      return ((pixel >> 16) & 0xFF) == 30 && ((pixel >> 8) & 0xFF) == 34 && (pixel & 0xFF) == 44;
    }, 2s);
    const uint32_t r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
    Check(r == 30 && g == 34 && b == 44, std::format("the top-left pixel is the client area's colour, not the frame ({}, {}, {})", r, g, b));
    Check(source.FramesCopied() >= 1, "the frame was copied on the render thread");
  });

  Scenario("An identical frame is dropped by the GPU check (test round 4)", [&] {
    RenderAndSample(*gpu, source, 0, 0);// settle: everything taken
    const auto unchanged = source.FramesUnchanged();
    const auto arrived = source.FramesArrived();
    // The click target repaints the same pixels
    InvalidateRect(window, nullptr, TRUE);
    WaitFor([&] { return source.FramesArrived() > arrived; }, 3s);
    Render::Readiness readiness = Render::Readiness::Pending;
    WaitFor([&] { readiness = source.Prepare(); return readiness != Render::Readiness::Pending; }, 2s);
    Check(source.FramesArrived() > arrived, "a repaint delivered a frame");
    Check(readiness == Render::Readiness::Unchanged, "Prepare says nothing changed");
    Check(source.FramesUnchanged() > unchanged, "and counts it as an identical frame skipped");
  });

  Scenario("A resized window: new size, frame pool re-created", [&] {
    SetWindowPos(window, nullptr, 0, 0, 900, 500, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT client {};
    GetClientRect(window, &client);
    const bool resized = WaitFor([&] {
      source.Prepare();
      const auto size = source.Size();
      return size.mWidth == static_cast<uint32_t>(client.right) && size.mHeight == static_cast<uint32_t>(client.bottom);
    }, 5s);
    const auto size = source.Size();
    Check(resized, std::format("the panel follows: {}x{} (client {}x{})", size.mWidth, size.mHeight, client.right, client.bottom));
  });

  Scenario("Pixel trims apply at once, with no new frame, and keep their pixels on a resize (CT-12)", [&] {
    RenderAndSample(*gpu, source, 0, 0);// take anything pending
    RECT client {};
    GetClientRect(window, &client);
    const auto copied = source.FramesCopied();
    source.SetCrop({10, 40, 20, 5});
    auto size = source.Size();
    Check(size.mWidth == static_cast<uint32_t>(client.right) - 30 && size.mHeight == static_cast<uint32_t>(client.bottom) - 45,
      std::format("trimmed at once: {}x{} (client {}x{})", size.mWidth, size.mHeight, client.right, client.bottom));
    Check(source.FramesCopied() == copied, "without waiting for a new frame");
    const auto untrimmed = source.UntrimmedSize();
    Check(untrimmed.mWidth == static_cast<uint32_t>(client.right) && untrimmed.mHeight == static_cast<uint32_t>(client.bottom),
      "the window's size before trimming is known (the settings window's ranges)");
    Check(RenderAndSample(*gpu, source, 0, 0) != 0, "and draws");
    // Not 700 x 450: the next scenario resizes to that to get a new frame
    SetWindowPos(window, nullptr, 0, 0, 760, 480, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(window, &client);
    const bool followed = WaitFor([&] {
      source.Prepare();
      size = source.Size();
      return size.mWidth == static_cast<uint32_t>(client.right) - 30 && size.mHeight == static_cast<uint32_t>(client.bottom) - 45;
    }, 5s);
    Check(followed, std::format("resized: the same pixels trimmed ({}x{}, client {}x{})", size.mWidth, size.mHeight, client.right, client.bottom));
    source.SetCrop({});
    size = source.Size();
    Check(size.mWidth == static_cast<uint32_t>(client.right), "untrimmed again at once");
  });

  Scenario("Inactive: frames are dropped, nothing copied (CP-5)", [&] {
    RenderAndSample(*gpu, source, 0, 0);// take anything pending
    source.SetActive(false);
    const auto copied = source.FramesCopied();
    const auto arrived = source.FramesArrived();
    SetWindowPos(window, nullptr, 0, 0, 700, 450, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    WaitFor([&] { return source.FramesArrived() > arrived; }, 3s);
    RenderAndSample(*gpu, source, 0, 0);
    Check(source.FramesArrived() > arrived, "frames still arrive (the session stays)");
    Check(source.FramesCopied() == copied, "but none is copied");
    source.SetActive(true);
  });

  Scenario("The window closes, then comes back: found again (CP-7)", [&] {
    target.Kill();
    Check(WaitFor([&] { return !source.IsCapturing(); }, 5s, [&] { source.Maintain(TimeUtil::Now()); }), "capture stops when the window closes");
    Target again;
    Check(WaitFor([&] { return source.IsCapturing(); }, 8s, [&] { source.Maintain(TimeUtil::Now()); }), "capture restarts on the reopened window");
    const auto arrived = source.FramesArrived();
    Check(WaitFor([&] { return source.FramesArrived() > arrived; }, 5s), "and frames arrive again");
  });

  return Finish();
}
