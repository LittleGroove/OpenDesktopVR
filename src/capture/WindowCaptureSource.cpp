// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Written from Microsoft's documentation of Windows.Graphics.Capture and its
// Win32 interop interfaces.

#include <OpenDesktopVR/D3D11Util.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
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

namespace Shaders {
#include <CompareCS.h>
}

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
using namespace std::chrono_literals;

namespace {

constexpr Render::PixelSize WaitingSize {800, 450};
constexpr auto RetryInterval = 1s;// CP-6: about once a second while missing
constexpr auto Format = wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
// CP-10: a window that changed this recently is "live" (a video, scrolling):
// its frames are drawn at once, without waiting for the GPU check
constexpr auto LiveWindow = 500ms;
// Two: three or four did no better under a game-like GPU load (test round 7)
constexpr int32_t PoolBuffers = 2;

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
  // The image shown, and a new frame being compared with it
  struct Image {
    winrt::com_ptr<ID3D11Texture2D> mTexture;
    winrt::com_ptr<ID3D11ShaderResourceView> mView;
    winrt::com_ptr<ID2D1Bitmap1> mBitmap;
    ID2D1DeviceContext2* mBitmapContext {nullptr};
    Render::PixelSize mSize {};
    // When Windows composed the frame (the same clock as TimeUtil::Now)
    TimeUtil::TimePoint mComposedAt {};
  };
  Image mShown;
  Image mCandidate;
  Render::Crop mCrop;// the user's trim (CT-12), applied when drawing
  bool mShownNotDrawn {false};
  bool mComparing {false};
  // Whether the check in flight holds the candidate back (a still window),
  // or only looks at a frame already drawn (a live one)
  bool mCompareGates {false};
  // When the check last found a change
  TimeUtil::TimePoint mLastChange {};
  winrt::com_ptr<ID3D11ComputeShader> mCompare;
  winrt::com_ptr<ID3D11Buffer> mResult;
  winrt::com_ptr<ID3D11UnorderedAccessView> mResultView;
  winrt::com_ptr<ID3D11Buffer> mReadback;
  std::unique_ptr<D3D11Util::PrivateContextState> mContextState;
  std::atomic<uint64_t> mUnchanged {0};
  std::atomic<uint64_t> mDrawnAtOnce {0};
  // From composition to drawn, per stats line (render thread only): a
  // steady delay keeps frames evenly spaced; a varying one is a hitch
  mutable uint64_t mLatencyCount {0};
  mutable double mLatencySumMs {0};
  mutable double mLatencyMaxMs {0};
  mutable uint64_t mLatencyOver10 {0};
  // Hitches (test round 7: "every second it loses a couple of frames"), per
  // drawn frame of a window that keeps changing: was the gap the window's
  // own (Windows composed it late), or ours (drawn much later than usual)?
  TimeUtil::TimePoint mPreviousComposed {};
  double mPreviousDelayMs {0};
  double mTypicalIntervalMs {0};// a running average of the window's frame interval
  std::atomic<uint64_t> mWindowGaps {0};
  std::atomic<uint64_t> mLateDraws {0};
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
  // The client area's size in the newest frame, before trimming
  std::atomic<uint32_t> mClientWidth {0};
  std::atomic<uint32_t> mClientHeight {0};

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
    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(mDevice, Format, PoolBuffers, size);
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
    const auto size = newest.ContentSize();
    RECT crop {0, 0, size.Width, size.Height};
    if (const auto client = WindowUtil::ClientCropInFrame(mWindow, static_cast<uint32_t>(size.Width), static_cast<uint32_t>(size.Height))) {
      crop = *client;
    }
    // Known even while nobody watches, for the settings window's trim
    // ranges (set up at the desk, before the game starts)
    mClientWidth = static_cast<uint32_t>(crop.right - crop.left);
    mClientHeight = static_cast<uint32_t>(crop.bottom - crop.top);
    if (!mActive) {
      newest.Close();// hidden or unwatched: nothing copied (CP-5)
      return;
    }
    // The whole client area is copied (CP-3); the user's trim is applied
    // when drawing, so changing it shows at once, even if the window sends
    // no new frame
    {
      std::scoped_lock lock(mMutex);
      if (size.Width != mPoolSize.Width || size.Height != mPoolSize.Height) {
        pool.Recreate(mDevice, Format, PoolBuffers, size);// the window was resized
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

  // The part of the shown image that's drawn: the client area, trimmed
  Render::PixelRect Kept() const {
    return mCrop.Apply(mShown.mSize);
  }

  Render::PixelSize Size() const {
    if (!mShown.mTexture) {
      return WaitingSize;
    }
    const auto kept = Kept();
    return {kept.mWidth, kept.mHeight};
  }

  bool EnsureImage(Image& image, Render::PixelSize size) {
    if (image.mTexture && image.mSize == size) {
      return true;
    }
    image = {};
    const D3D11_TEXTURE2D_DESC desc {
      .Width = size.mWidth, .Height = size.mHeight, .MipLevels = 1, .ArraySize = 1,
      .Format = DXGI_FORMAT_B8G8R8A8_UNORM, .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
    };
    if (FAILED(mGpu.Device()->CreateTexture2D(&desc, nullptr, image.mTexture.put()))
        || FAILED(mGpu.Device()->CreateShaderResourceView(image.mTexture.get(), nullptr, image.mView.put()))) {
      image = {};
      return false;
    }
    image.mSize = size;
    return true;
  }

  bool EnsureCompare() {
    if (mCompare) {
      return true;
    }
    mContextState = std::make_unique<D3D11Util::PrivateContextState>(mGpu.Device());
    const D3D11_BUFFER_DESC resultDesc {
      .ByteWidth = 16, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_UNORDERED_ACCESS,
      .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
    const D3D11_BUFFER_DESC readbackDesc {
      .ByteWidth = 16, .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ};
    D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc {.Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D11_UAV_DIMENSION_BUFFER};
    viewDesc.Buffer = {0, 4, D3D11_BUFFER_UAV_FLAG_RAW};
    if (FAILED(mGpu.Device()->CreateComputeShader(Shaders::g_CompareCS, sizeof(Shaders::g_CompareCS), nullptr, mCompare.put()))
        || FAILED(mGpu.Device()->CreateBuffer(&resultDesc, nullptr, mResult.put()))
        || FAILED(mGpu.Device()->CreateUnorderedAccessView(mResult.get(), &viewDesc, mResultView.put()))
        || FAILED(mGpu.Device()->CreateBuffer(&readbackDesc, nullptr, mReadback.put()))) {
      mCompare = nullptr;
      return false;
    }
    return true;
  }

  // The candidate becomes what's shown
  void Accept() {
    std::swap(mShown, mCandidate);
    mShownNotDrawn = true;
  }

  // Starts the GPU comparison of the candidate with what's shown
  bool StartCompare() {
    if (!EnsureCompare()) {
      return false;
    }
    auto* context = mGpu.Context();
    {
      const auto scope = mContextState->Enter(context);
      const UINT zeros[4] {};
      context->ClearUnorderedAccessViewUint(mResultView.get(), zeros);
      ID3D11ShaderResourceView* views[] {mCandidate.mView.get(), mShown.mView.get()};
      ID3D11UnorderedAccessView* uav = mResultView.get();
      context->CSSetShader(mCompare.get(), nullptr, 0);
      context->CSSetShaderResources(0, 2, views);
      context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
      context->Dispatch((mCandidate.mSize.mWidth + 7) / 8, (mCandidate.mSize.mHeight + 7) / 8, 1);
      context->CopyResource(mReadback.get(), mResult.get());
    }
    context->Flush();
    mComparing = true;
    return true;
  }

  // CP-10. The GPU check's answer is read back, and with a game running the
  // GPU gets to it late: about 44 ms in test round 7, so a video waiting for
  // it drew 18 of its 30 frames a second. So only a still window waits for
  // it (most of its frames are identical: dropped). A live one's frames are
  // drawn at once; the check runs alongside, one at a time, only to notice
  // when the window has gone still.
  Render::Readiness Prepare() {
    auto* context = mGpu.Context();
    const auto now = TimeUtil::Now();
    if (mComparing) {
      D3D11_MAPPED_SUBRESOURCE mapped {};
      const auto hr = context->Map(mReadback.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (mCompareGates) {
          return Render::Readiness::Pending;
        }
        // Checking a frame already drawn: nothing waits for it
      } else {
        mComparing = false;
        bool changed = true;// if the answer can't be read, assume a change
        if (SUCCEEDED(hr)) {
          changed = *static_cast<const uint32_t*>(mapped.pData) != 0;
          context->Unmap(mReadback.get(), 0);
        }
        if (changed) {
          mLastChange = now;
        }
        if (mCompareGates) {
          if (changed) {
            Accept();
            return Render::Readiness::Ready;
          }
          ++mUnchanged;// identical: dropped
        }
      }
    }

    wgc::Direct3D11CaptureFrame frame {nullptr};
    RECT crop {};
    {
      std::scoped_lock lock(mMutex);
      frame = std::exchange(mPending, nullptr);
      crop = mPendingCrop;
    }
    if (frame && !mActive) {
      // Arrived just before the panel went inactive: dropped too (CP-5)
      frame.Close();
      frame = nullptr;
    }
    if (frame) {
      winrt::com_ptr<ID3D11Texture2D> source;
      auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
      const Render::PixelSize size {static_cast<uint32_t>(crop.right - crop.left), static_cast<uint32_t>(crop.bottom - crop.top)};
      if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void())) && EnsureImage(mCandidate, size)) {
        // CP-3: only the client area, never the whole (possibly monitor-sized) surface
        const D3D11_BOX box {static_cast<UINT>(crop.left), static_cast<UINT>(crop.top), 0, static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1};
        context->CopySubresourceRegion(mCandidate.mTexture.get(), 0, 0, 0, 0, source.get(), 0, &box);
        mCandidate.mComposedAt
          = TimeUtil::TimePoint {std::chrono::duration_cast<TimeUtil::Duration>(frame.SystemRelativeTime())};
        ++mCopied;
        frame.Close();
        if (!mShown.mTexture || !(mShown.mSize == size)) {
          Accept();// nothing comparable: a first frame, a new size
          return Render::Readiness::Ready;
        }
        if (now - mLastChange < LiveWindow) {
          if (!mComparing && StartCompare()) {
            mCompareGates = false;
          }
          Accept();
          ++mDrawnAtOnce;
          return Render::Readiness::Ready;
        }
        if (!StartCompare()) {
          Accept();
          return Render::Readiness::Ready;
        }
        mCompareGates = true;
        return Render::Readiness::Pending;
      }
      frame.Close();
    }
    return mShownNotDrawn ? Render::Readiness::Ready : Render::Readiness::Unchanged;
  }

  // A frame being drawn, composed at `composed`, `delayMs` after that
  void CountHitch(TimeUtil::TimePoint composed, double delayMs) {
    const double interval = TimeUtil::ToMilliseconds(composed - mPreviousComposed);
    // Only within a run of frames: a window that sat still for a while
    // before changing again isn't hitching
    if (mPreviousComposed != TimeUtil::TimePoint {} && interval > 0 && interval <= 250) {
      if (mTypicalIntervalMs > 0) {
        // Much longer than usual: the window (or Windows) skipped frames
        if (interval > mTypicalIntervalMs * 1.6 && interval > mTypicalIntervalMs + 10) {
          ++mWindowGaps;
        }
        // Drawn more than about half a frame later than the frame before
        if (delayMs - mPreviousDelayMs > std::max(8.0, mTypicalIntervalMs * 0.6)) {
          ++mLateDraws;
        }
        mTypicalIntervalMs += (std::min(interval, mTypicalIntervalMs * 3) - mTypicalIntervalMs) * 0.1;
      } else {
        mTypicalIntervalMs = interval;
      }
    }
    mPreviousComposed = composed;
    mPreviousDelayMs = delayMs;
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

Render::Readiness WindowCaptureSource::Prepare() {
  return mState->Prepare();
}

void WindowCaptureSource::Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint) {
  auto& state = *mState;
  auto& shown = state.mShown;
  context->Clear({0.118f, 0.141f, 0.188f, 1});
  if (shown.mTexture && (!shown.mBitmap || shown.mBitmapContext != context)) {
    winrt::com_ptr<IDXGISurface> surface;
    shown.mTexture->QueryInterface(surface.put());
    const D2D1_BITMAP_PROPERTIES1 properties {
      .pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, .dpiX = 96, .dpiY = 96};
    shown.mBitmap = nullptr;
    context->CreateBitmapFromDxgiSurface(surface.get(), &properties, shown.mBitmap.put());
    shown.mBitmapContext = context;
  }
  if (shown.mBitmap) {
    const D2D1_RECT_F destination {0, 0, static_cast<float>(size.mWidth), static_cast<float>(size.mHeight)};
    const auto kept = state.Kept();
    const D2D1_RECT_F source {static_cast<float>(kept.mLeft), static_cast<float>(kept.mTop),
      static_cast<float>(kept.mLeft + kept.mWidth), static_cast<float>(kept.mTop + kept.mHeight)};
    const bool exact = kept.mWidth == size.mWidth && kept.mHeight == size.mHeight;
    context->DrawBitmap(shown.mBitmap.get(), &destination, 1.0f,
      exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &source);
    if (state.mShownNotDrawn && shown.mComposedAt != TimeUtil::TimePoint {}) {
      const double latency = TimeUtil::ToMilliseconds(TimeUtil::Now() - shown.mComposedAt);
      if (latency >= 0 && latency < 10'000) {
        ++state.mLatencyCount;
        state.mLatencySumMs += latency;
        state.mLatencyMaxMs = std::max(state.mLatencyMaxMs, latency);
        state.mLatencyOver10 += latency > 10 ? 1 : 0;
        state.CountHitch(shown.mComposedAt, latency);
      }
    }
    state.mShownNotDrawn = false;
  } else {
    state.DrawWaiting(context, size);
  }
}

std::string WindowCaptureSource::StatsText() const {
  auto& state = *mState;
  auto text = std::format("frames {} arrived, {} identical skipped, {} drawn without waiting for the check", state.mArrived.load(),
    state.mUnchanged.load(), state.mDrawnAtOnce.load());
  if (state.mLatencyCount > 0) {
    text += std::format(", drawn {:.1f} ms after Windows composed them on average, at most {:.1f} ms ({} over 10 ms)",
      state.mLatencySumMs / static_cast<double>(state.mLatencyCount), state.mLatencyMaxMs, state.mLatencyOver10);
  }
  // Totals, like the frame counts
  text += std::format("; hitches: {} late from Windows (the window or its capture), {} drawn late by us",
    state.mWindowGaps.load(), state.mLateDraws.load());
  // Per stats line
  state.mLatencyCount = 0;
  state.mLatencySumMs = 0;
  state.mLatencyMaxMs = 0;
  state.mLatencyOver10 = 0;
  return text;
}

uint64_t WindowCaptureSource::FramesUnchanged() const noexcept {
  return mState->mUnchanged.load();
}

uint64_t WindowCaptureSource::FramesDrawnAtOnce() const noexcept {
  return mState->mDrawnAtOnce.load();
}

uint64_t WindowCaptureSource::WindowGaps() const noexcept {
  return mState->mWindowGaps.load();
}

uint64_t WindowCaptureSource::LateDraws() const noexcept {
  return mState->mLateDraws.load();
}

void WindowCaptureSource::SetActive(bool active) {
  mState->mActive = active;
}

void WindowCaptureSource::SetCrop(const Render::Crop& crop) {
  mState->mCrop = crop;// render thread, like Size() and Render()
}

Render::PixelSize WindowCaptureSource::UntrimmedSize() const {
  return {mState->mClientWidth.load(), mState->mClientHeight.load()};
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
      // Elevated windows are a top support topic (CMP-8): log what we saw
      Log::Info(
        "Capture: started on '{}' (its process: {})",
        StringUtil::Narrow(window->mTitle),
        ProcessUtil::ElevationName(ProcessUtil::ProcessElevation(window->mProcessID)));
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
