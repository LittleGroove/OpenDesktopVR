// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Written from Microsoft's documentation of Windows.Graphics.Capture and its
// Win32 interop interfaces.

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/WindowCaptureSource.hpp>

#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <atomic>
#include <format>
#include <mutex>

namespace OpenDesktopVR::Capture {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
using namespace std::chrono_literals;

namespace {

constexpr Render::PixelSize WaitingSize {800, 450};
constexpr auto RetryInterval = 1s;// CP-6: about once a second while missing
constexpr auto Format = wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;

bool HasProperty(const wchar_t* type, const wchar_t* property) noexcept {
  try {
    return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(type, property);
  } catch (...) {
    return false;
  }
}

}// namespace

bool IsCaptureSupported() noexcept {
  try {
    return wgc::GraphicsCaptureSession::IsSupported();
  } catch (...) {
    return false;
  }
}

struct WindowCaptureSource::State : std::enable_shared_from_this<State> {
  State(Render::GpuDevice& gpu, WindowCaptureOptions options, Render::MarkDirty markDirty)
    : mGpu(gpu), mOptions(std::move(options)), mMarkDirty(std::move(markDirty)) {
  }

  Render::GpuDevice& mGpu;
  WindowCaptureOptions mOptions;
  Render::MarkDirty mMarkDirty;
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice mDevice {nullptr};

  // Owned by the render thread
  HWND mWindow {nullptr};
  wgc::GraphicsCaptureItem mItem {nullptr};
  wgc::Direct3D11CaptureFramePool mPool {nullptr};
  wgc::GraphicsCaptureSession mSession {nullptr};
  wgc::Direct3D11CaptureFramePool::FrameArrived_revoker mFrameRevoker;
  wgc::GraphicsCaptureItem::Closed_revoker mClosedRevoker;
  TimeUtil::TimePoint mNextAttempt {};
  std::wstring mStatus {L"starting"};
  winrt::com_ptr<ID3D11Texture2D> mImage;
  winrt::com_ptr<ID2D1Bitmap1> mBitmap;
  ID2D1DeviceContext2* mBitmapContext {nullptr};
  Render::PixelSize mImageSize {};
  winrt::com_ptr<IDWriteTextFormat> mTextFormat;
  winrt::com_ptr<ID2D1SolidColorBrush> mBrush;

  // Shared with the capture callback thread
  mutable std::mutex mMutex;
  wgc::Direct3D11CaptureFrame mPending {nullptr};
  RECT mPendingCrop {};
  winrt::Windows::Graphics::SizeInt32 mPoolSize {};
  std::atomic<bool> mActive {true};
  std::atomic<bool> mClosed {false};
  std::atomic<uint64_t> mArrived {0};
  std::atomic<uint64_t> mCopied {0};

  bool EnsureDevice() {
    if (mDevice) {
      return true;
    }
    winrt::com_ptr<IDXGIDevice> dxgi;
    if (FAILED(mGpu.Device()->QueryInterface(dxgi.put()))) {
      return false;
    }
    winrt::com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()))) {
      return false;
    }
    mDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return true;
  }

  void Stop() noexcept {
    mFrameRevoker.revoke();
    mClosedRevoker.revoke();
    try {
      if (mSession) {
        mSession.Close();
      }
      if (mPool) {
        mPool.Close();
      }
    } catch (...) {
    }
    mSession = nullptr;
    mPool = nullptr;
    mItem = nullptr;
    mWindow = nullptr;
    std::scoped_lock lock(mMutex);
    if (mPending) {
      mPending.Close();
      mPending = nullptr;
    }
  }

  bool Start(HWND window) {
    if (!EnsureDevice()) {
      mStatus = L"no capture device";
      return false;
    }
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item {nullptr};
    if (FAILED(interop->CreateForWindow(window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item))) || !item) {
      return false;
    }
    const auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) {
      return false;// still opening: retried (CP-7)
    }
    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(mDevice, Format, 2, size);
    auto session = pool.CreateCaptureSession(item);
    // We draw our own pointer feedback
    if (HasProperty(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) {
      session.IsCursorCaptureEnabled(false);
    }
    // No yellow border where Windows allows it (Windows 11)
    if (HasProperty(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
      try {
        session.IsBorderRequired(false);
      } catch (...) {
      }
    }
    std::weak_ptr<State> weak = weak_from_this();
    mFrameRevoker = pool.FrameArrived(winrt::auto_revoke, [weak](const wgc::Direct3D11CaptureFramePool& sender, auto&&) {
      if (auto self = weak.lock()) {
        ThreadUtil::CatchAll("capture frame", [&] { self->OnFrame(sender); });
      }
    });
    mClosedRevoker = item.Closed(winrt::auto_revoke, [weak](auto&&, auto&&) {
      if (auto self = weak.lock()) {
        self->mClosed = true;
        self->mMarkDirty();
      }
    });
    {
      std::scoped_lock lock(mMutex);
      mPoolSize = size;
    }
    session.StartCapture();
    mWindow = window;
    mItem = item;
    mPool = pool;
    mSession = session;
    return true;
  }

  // On the capture callback thread
  void OnFrame(const wgc::Direct3D11CaptureFramePool& pool) {
    // CP-1: drain to the newest frame
    wgc::Direct3D11CaptureFrame newest {nullptr};
    while (auto frame = pool.TryGetNextFrame()) {
      if (newest) {
        newest.Close();
      }
      newest = frame;
    }
    if (!newest) {
      return;
    }
    ++mArrived;
    if (!mActive) {
      newest.Close();// hidden or unwatched: nothing copied (CP-5)
      return;
    }
    const auto size = newest.ContentSize();
    RECT crop {0, 0, size.Width, size.Height};
    if (const auto client = WindowUtil::ClientCropInFrame(mWindow, static_cast<uint32_t>(size.Width), static_cast<uint32_t>(size.Height))) {
      crop = *client;
    }
    {
      std::scoped_lock lock(mMutex);
      if (size.Width != mPoolSize.Width || size.Height != mPoolSize.Height) {
        pool.Recreate(mDevice, Format, 2, size);// the window was resized
        mPoolSize = size;
      }
      if (mPending) {
        mPending.Close();
      }
      mPending = newest;
      mPendingCrop = crop;
    }
    mMarkDirty();
  }

  Render::PixelSize Size() const {
    std::scoped_lock lock(mMutex);
    if (mPending) {
      return {static_cast<uint32_t>(mPendingCrop.right - mPendingCrop.left), static_cast<uint32_t>(mPendingCrop.bottom - mPendingCrop.top)};
    }
    return mImage ? mImageSize : WaitingSize;
  }

  void TakePending(ID2D1DeviceContext2* context) {
    wgc::Direct3D11CaptureFrame frame {nullptr};
    RECT crop {};
    {
      std::scoped_lock lock(mMutex);
      frame = std::exchange(mPending, nullptr);
      crop = mPendingCrop;
    }
    if (!frame) {
      return;
    }
    winrt::com_ptr<ID3D11Texture2D> source;
    auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()))) {
      frame.Close();
      return;
    }
    const Render::PixelSize size {static_cast<uint32_t>(crop.right - crop.left), static_cast<uint32_t>(crop.bottom - crop.top)};
    if (!mImage || !(mImageSize == size) || mBitmapContext != context) {
      mImage = nullptr;
      mBitmap = nullptr;
      const D3D11_TEXTURE2D_DESC desc {
        .Width = size.mWidth, .Height = size.mHeight, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM, .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
      };
      winrt::com_ptr<IDXGISurface> surface;
      if (FAILED(mGpu.Device()->CreateTexture2D(&desc, nullptr, mImage.put())) || FAILED(mImage->QueryInterface(surface.put()))) {
        mImage = nullptr;
        frame.Close();
        return;
      }
      const D2D1_BITMAP_PROPERTIES1 properties {
        .pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, .dpiX = 96, .dpiY = 96};
      context->CreateBitmapFromDxgiSurface(surface.get(), &properties, mBitmap.put());
      mBitmapContext = context;
      mImageSize = size;
    }
    // CP-3: only the client area, never the whole (possibly monitor-sized) surface
    const D3D11_BOX box {static_cast<UINT>(crop.left), static_cast<UINT>(crop.top), 0, static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1};
    mGpu.Context()->CopySubresourceRegion(mImage.get(), 0, 0, 0, 0, source.get(), 0, &box);
    frame.Close();
    ++mCopied;
  }

  void DrawWaiting(ID2D1DeviceContext2* context, Render::PixelSize size) {
    if (!mTextFormat) {
      mGpu.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 30, L"", mTextFormat.put());
    }
    if (!mBrush) {
      context->CreateSolidColorBrush({0.94f, 0.94f, 0.94f, 1}, mBrush.put());
    }
    const auto text = std::format(L"{}\n{}", mOptions.mName, mStatus);
    if (mTextFormat && mBrush) {
      context->DrawText(text.c_str(), static_cast<UINT32>(text.size()), mTextFormat.get(),
        {40, 40, static_cast<float>(size.mWidth) - 40, static_cast<float>(size.mHeight) - 40}, mBrush.get());
    }
  }
};

WindowCaptureSource::WindowCaptureSource(Render::GpuDevice& gpu, WindowCaptureOptions options, Render::MarkDirty markDirty)
  : mState(std::make_shared<State>(gpu, std::move(options), std::move(markDirty))) {
  Maintain(TimeUtil::Now());
}

WindowCaptureSource::~WindowCaptureSource() {
  mState->Stop();
}

Render::PixelSize WindowCaptureSource::Size() const {
  return mState->Size();
}

void WindowCaptureSource::Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint) {
  auto& state = *mState;
  state.TakePending(context);
  context->Clear({0.118f, 0.141f, 0.188f, 1});
  if (state.mBitmap && state.mBitmapContext == context) {
    const D2D1_RECT_F destination {0, 0, static_cast<float>(size.mWidth), static_cast<float>(size.mHeight)};
    const bool exact = state.mImageSize == size;
    context->DrawBitmap(state.mBitmap.get(), &destination, 1.0f,
      exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
  } else {
    state.DrawWaiting(context, size);
  }
}

void WindowCaptureSource::SetActive(bool active) {
  mState->mActive = active;
}

void WindowCaptureSource::Maintain(TimeUtil::TimePoint now) {
  auto& state = *mState;
  if (state.mClosed.exchange(false) || (state.mWindow && !IsWindow(state.mWindow))) {
    Log::Info("Capture: '{}' closed; waiting for it to come back", StringUtil::Narrow(state.mOptions.mName));
    state.Stop();
    state.mStatus = L"The window closed. Waiting for it to come back...";
    state.mNextAttempt = now;
    state.mMarkDirty();
  }
  if (state.mSession || now < state.mNextAttempt) {
    return;// found: no more checks (CP-6)
  }
  state.mNextAttempt = now + RetryInterval;
  ThreadUtil::CatchAll("capture start", [&] {
    const auto window = WindowMatch::FindWindow(state.mOptions.mRule);
    if (!window) {
      state.mStatus = L"Waiting for the window to open...";
      return;
    }
    if (state.Start(window->mWindow)) {
      state.mStatus = L"Capturing";
      Log::Info("Capture: started on '{}'", StringUtil::Narrow(window->mTitle));
    } else {
      state.mStatus = L"The window isn't ready yet. Retrying...";// CP-7
    }
  });
}

std::wstring WindowCaptureSource::Describe() const {
  return mState->mStatus;
}

uint64_t WindowCaptureSource::FramesArrived() const noexcept {
  return mState->mArrived.load();
}

uint64_t WindowCaptureSource::FramesCopied() const noexcept {
  return mState->mCopied.load();
}

bool WindowCaptureSource::IsCapturing() const noexcept {
  return mState->mSession != nullptr;
}

}// namespace OpenDesktopVR::Capture
