// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// odvr-mirror: shows, in a desktop window, the panel images a layer receives
// from the host app (doc 02 "Mirror viewer"). For checking panels without a
// headset. It connects like a layer (same host link, handles pulled from the
// host) but never writes the layer's report.
//
//   odvr-mirror.exe [--screenshot <png>]
//
// --screenshot: once panels have shown for 2 s, save the window as a PNG
// and exit (for checking the host's drawing without looking)

#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/HostLink.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include "save-png.hpp"

#include <Windows.h>

#include <dxgi1_2.h>
#include <shellapi.h>

#include <array>
#include <filesystem>
#include <optional>
#include <vector>
#include <format>
#include <cstring>
#include <map>
#include <string>

using namespace OpenDesktopVR;

namespace {

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.Mirror";

struct CachedBitmaps {
  uint64_t mGeneration {0};
  uint32_t mTextureGeneration {0};
  std::array<winrt::com_ptr<ID2D1Bitmap1>, SharedAbi::RingSlots> mSlots;
};

class Mirror {
 public:
  bool Create(HINSTANCE instance) {
    mGpu = Render::GpuDevice::Create({.mAllowWarp = true});
    if (!mGpu) {
      return false;
    }
    mLink = std::make_unique<HostLink::Client>(mGpu->Device(), HostLink::ClientOptions {.mWritesStatus = false});

    WNDCLASSEXW windowClass {sizeof(windowClass)};
    windowClass.lpfnWndProc = &Mirror::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = WindowClass;
    RegisterClassExW(&windowClass);
    const auto title = std::format(L"{} mirror: {}", Product::DisplayName, Product::Tagline);
    mWindow = CreateWindowExW(0, WindowClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
      CW_USEDEFAULT, 1280, 720, nullptr, nullptr, instance, this);
    if (!mWindow) {
      return false;
    }

    winrt::com_ptr<IDXGIDevice1> dxgiDevice;
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::com_ptr<IDXGIFactory2> factory;
    mGpu->Device()->QueryInterface(dxgiDevice.put());
    // One queued frame: the image drawn is the one just read, before the
    // host's ring comes back round to that slot
    dxgiDevice->SetMaximumFrameLatency(1);
    dxgiDevice->GetAdapter(adapter.put());
    adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
    const DXGI_SWAP_CHAIN_DESC1 desc {
      .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .SampleDesc = {1, 0},
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = 2,
      .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
    };
    if (FAILED(factory->CreateSwapChainForHwnd(mGpu->Device(), mWindow, &desc, nullptr, nullptr, mSwapchain.put()))) {
      return false;
    }
    mGpu->DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 15, L"", mTextFormat.put());
    mGpu->D2D()->CreateSolidColorBrush({0.94f, 0.94f, 0.94f, 1}, mBrush.put());
    return true;
  }

  int Run(const std::optional<std::filesystem::path>& screenshot) {
    MSG message {};
    std::optional<TimeUtil::TimePoint> panelsSince;
    for (;;) {
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
          return static_cast<int>(message.wParam);
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      Draw();
      if (screenshot) {
        if (mLink->Panels().empty()) {
          panelsSince.reset();
        } else if (!panelsSince) {
          panelsSince = TimeUtil::Now();
        } else if (TimeUtil::Now() - *panelsSince > std::chrono::seconds(2)) {
          SaveScreenshot(*screenshot);
          return 0;
        }
      }
      mSwapchain->Present(1, 0);// vsync paces the loop
    }
  }

  void SaveScreenshot(const std::filesystem::path& path) {
    winrt::com_ptr<ID3D11Texture2D> back;
    mSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
    D3D11_TEXTURE2D_DESC desc {};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    mGpu->Device()->CreateTexture2D(&desc, nullptr, staging.put());
    mGpu->Context()->CopyResource(staging.get(), back.get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(mGpu->Context()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return;
    }
    std::vector<uint8_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
      std::memcpy(pixels.data() + static_cast<std::size_t>(y) * desc.Width * 4,
        static_cast<const uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, desc.Width * 4);
    }
    mGpu->Context()->Unmap(staging.get(), 0);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
      pixels[i] = 0xFF;// the window has no alpha
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestUtilities::SavePNG(path, desc.Width, desc.Height, pixels);
  }

 private:
  void ResizeTarget() {
    mTarget = nullptr;
    mGpu->D2D()->SetTarget(nullptr);
    mSwapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
  }

  ID2D1Bitmap1* TargetBitmap() {
    if (!mTarget) {
      winrt::com_ptr<IDXGISurface> surface;
      mSwapchain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void());
      const D2D1_BITMAP_PROPERTIES1 properties {
        .pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE},
        .dpiX = 96, .dpiY = 96,
        .bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      };
      mGpu->D2D()->CreateBitmapFromDxgiSurface(surface.get(), &properties, mTarget.put());
    }
    return mTarget.get();
  }

  ID2D1Bitmap1* SlotBitmap(const SharedAbi::PanelEntry& entry, const HostLink::PanelResources& resources) {
    auto& cache = mBitmaps[entry.mPanelID];
    if (cache.mGeneration != resources.mGeneration || cache.mTextureGeneration != entry.mTextureGeneration) {
      cache = {resources.mGeneration, entry.mTextureGeneration, {}};
    }
    auto& bitmap = cache.mSlots[entry.mCurrentSlot];
    if (!bitmap) {
      winrt::com_ptr<IDXGISurface> surface;
      resources.mTextures[entry.mCurrentSlot]->QueryInterface(surface.put());
      const D2D1_BITMAP_PROPERTIES1 properties {
        .pixelFormat = {static_cast<DXGI_FORMAT>(entry.mTextureFormat), D2D1_ALPHA_MODE_PREMULTIPLIED},
        .dpiX = 96, .dpiY = 96,
      };
      mGpu->D2D()->CreateBitmapFromDxgiSurface(surface.get(), &properties, bitmap.put());
    }
    return bitmap.get();
  }

  void Draw() {
    mLink->Update(TimeUtil::Now());
    auto* d2d = mGpu->D2D();
    d2d->SetTarget(TargetBitmap());
    d2d->BeginDraw();
    d2d->Clear({0.118f, 0.141f, 0.188f, 1});
    const auto size = d2d->GetSize();

    const auto panels = mLink->Panels();
    const auto status = mLink->IsConnected()
      ? std::format(L"Connected to the host app: {} panel(s)", panels.size())
      : std::wstring {L"The host app isn't running"};
    d2d->DrawText(status.c_str(), static_cast<UINT32>(status.size()), mTextFormat.get(), {12, 8, size.width, 32}, mBrush.get());

    const float margin = 12;
    const float top = 40;
    const float cellWidth = panels.empty() ? size.width : (size.width - margin) / static_cast<float>(panels.size());
    for (std::size_t i = 0; i < panels.size(); ++i) {
      const auto& entry = panels[i];
      const float left = margin + cellWidth * static_cast<float>(i);
      const auto label = std::format(L"Panel {:X}: version {}{}", entry.mPanelID, entry.mContentVersion,
        (entry.mFlags & SharedAbi::PanelFlags::Visible) ? L"" : L" (hidden)");
      d2d->DrawText(label.c_str(), static_cast<UINT32>(label.size()), mTextFormat.get(),
        {left, top, left + cellWidth - margin, top + 24}, mBrush.get());
      const auto* resources = mLink->Resources(entry);
      if (!resources || entry.mContentWidth == 0 || entry.mCurrentSlot >= SharedAbi::RingSlots) {
        continue;
      }
      // GPU-side wait: the tool may wait, the image must be complete
      mGpu->Context()->Wait(resources->mFence.get(), entry.mFenceValue);
      auto* bitmap = SlotBitmap(entry, *resources);
      if (!bitmap) {
        continue;
      }
      const float w = cellWidth - margin;
      const float h = w * static_cast<float>(entry.mContentHeight) / static_cast<float>(entry.mContentWidth);
      const D2D1_RECT_F destination {left, top + 28, left + w, top + 28 + h};
      const D2D1_RECT_F source {0, 0, static_cast<float>(entry.mContentWidth), static_cast<float>(entry.mContentHeight)};
      d2d->DrawBitmap(bitmap, &destination, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &source);
    }
    d2d->EndDraw();
    d2d->SetTarget(nullptr);
  }

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<Mirror*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
      case WM_SIZE:
        if (self && self->mSwapchain) {
          self->ResizeTarget();
        }
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
  }

  std::unique_ptr<Render::GpuDevice> mGpu;
  std::unique_ptr<HostLink::Client> mLink;
  HWND mWindow {nullptr};
  winrt::com_ptr<IDXGISwapChain1> mSwapchain;
  winrt::com_ptr<ID2D1Bitmap1> mTarget;
  winrt::com_ptr<IDWriteTextFormat> mTextFormat;
  winrt::com_ptr<ID2D1SolidColorBrush> mBrush;
  std::map<uint64_t, CachedBitmaps> mBitmaps;
};

}// namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  Log::Config config;
  if (const auto folder = Product::LogsFolder(); !folder.empty()) {
    config.mFile = folder / L"mirror.log";
  }
  Log::Start(config);
  std::optional<std::filesystem::path> screenshot;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i + 1 < argc; ++i) {
    if (std::wstring_view {argv[i]} == L"--screenshot") {
      screenshot = std::filesystem::path {argv[i + 1]};
    }
  }
  LocalFree(argv);
  Mirror mirror;
  int code = 1;
  if (mirror.Create(instance)) {
    code = mirror.Run(screenshot);
  } else {
    MessageBoxW(nullptr, L"The mirror couldn't start its GPU window. See mirror.log.", Product::DisplayName.data(), MB_ICONERROR);
  }
  Log::Stop();
  return code;
}
