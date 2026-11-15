// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/FrameWakeup.hpp>
#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Publisher.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <Windows.h>

#include <array>
#include <format>
#include <mutex>
#include <optional>

namespace OpenDesktopVR::Host {

using namespace std::chrono_literals;

namespace {

// Ring textures are typed UNORM (the bytes are sRGB-encoded); the layer's
// swapchain is the sRGB variant of the same family, so it can copy
DXGI_FORMAT RingFormatFor(uint32_t preferredPanelFormat) {
  switch (static_cast<DXGI_FORMAT>(preferredPanelFormat)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
  }
}

}// namespace

struct RenderLoop::Impl {
  RenderLoopOptions mOptions;
  std::vector<std::pair<PanelDefinition, SourceFactory>> mDefinitions;
  std::vector<HostPanel> mPanels;
  std::unique_ptr<Render::GpuDevice> mGpu;
  std::optional<Publisher> mPublisher;
  DXGI_FORMAT mRingFormat {DXGI_FORMAT_B8G8R8A8_UNORM};
  bool mLastVisible {true};

  std::thread mThread;
  HANDLE mStop {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE mTimer {CreateWaitableTimerW(nullptr, FALSE, nullptr)};

  mutable std::mutex mStatsMutex;
  RenderStats mStats;
  TimeUtil::PeriodicTimer mStatsTimer {60s};
  TimeUtil::PeriodicTimer mStatusTimer {1s};
  TimeUtil::TimePoint mNextPublisherAttempt {};
  SharedMemory::Status mLastBlockStatus {SharedMemory::Status::Ok};
  uint64_t mRendersSinceStats {0};
  uint64_t mFramesSinceStats {0};

  ~Impl() {
    CloseHandle(mStop);
    CloseHandle(mTimer);
  }

  void Main(const std::atomic<bool>& visible);
  bool EnsureGpu(std::optional<LUID> adapter);
  void EnsurePublisher(TimeUtil::TimePoint now);
  void FollowLayer();
  bool RenderPanel(HostPanel& panel, TimeUtil::TimePoint now);
  void Publish(bool visible);
  SharedAbi::PanelEntry EntryFor(const HostPanel& panel, bool visible) const;
  void WriteStatsLine();
};

RenderLoop::RenderLoop(RenderLoopOptions options) : mImpl(std::make_unique<Impl>()) {
  mImpl->mOptions = options;
}

RenderLoop::~RenderLoop() {
  Stop();
}

void RenderLoop::AddPanel(PanelDefinition definition, SourceFactory makeSource) {
  mImpl->mDefinitions.emplace_back(std::move(definition), std::move(makeSource));
}

void RenderLoop::Start() {
  if (mImpl->mThread.joinable()) {
    return;
  }
  mImpl->mThread = std::thread([this] {
    ThreadUtil::SetCurrentThreadName(L"Panel renderer");
    ThreadUtil::CatchAll("panel render thread", [this] { mImpl->Main(mPanelsVisible); });
  });
}

void RenderLoop::Stop() noexcept {
  if (!mImpl || !mImpl->mThread.joinable()) {
    return;
  }
  SetEvent(mImpl->mStop);
  mImpl->mThread.join();
}

void RenderLoop::SetPanelsVisible(bool visible) noexcept {
  mPanelsVisible.store(visible);
  FrameWakeup::RequestFrameSoon();
}

RenderStats RenderLoop::Stats() const noexcept {
  std::scoped_lock lock(mImpl->mStatsMutex);
  return mImpl->mStats;
}

bool RenderLoop::Impl::EnsureGpu(std::optional<LUID> adapter) {
  if (mGpu && (!adapter || Render::SameLuid(mGpu->AdapterLuid(), *adapter))) {
    return true;
  }
  auto gpu = Render::GpuDevice::Create({.mAdapter = adapter, .mAllowWarp = mOptions.mAllowWarp});
  if (!gpu) {
    return mGpu != nullptr;
  }
  // Everything shared is re-created on the new device: layers drop and
  // re-open it all when the generation changes
  for (auto& panel: mPanels) {
    panel.mRing.reset();
    panel.mSource.reset();
  }
  mGpu = std::move(gpu);
  for (std::size_t i = 0; i < mPanels.size(); ++i) {
    mPanels[i].mSource = mDefinitions[i].second(*mGpu);
    mPanels[i].mDirty.Mark(Panels::Dirty::Structural);
  }
  if (mPublisher) {
    mPublisher->BumpGeneration();
  }
  std::scoped_lock lock(mStatsMutex);
  ++mStats.mDeviceCreations;
  return true;
}

void RenderLoop::Impl::EnsurePublisher(TimeUtil::TimePoint now) {
  if (mPublisher || now < mNextPublisherAttempt) {
    return;
  }
  SharedMemory::Status status {};
  mPublisher = Publisher::Create(status);
  if (mPublisher) {
    Log::Info("Shared memory created; layers can attach");
    for (auto& panel: mPanels) {
      panel.mDirty.Mark(Panels::Dirty::Structural);
    }
    std::scoped_lock lock(mStatsMutex);
    mStats.mBlockCreated = true;
    return;
  }
  // "Already exists" usually means a layer still holds the previous host's
  // block: it lets go within 250 ms of noticing, so retry soon
  mNextPublisherAttempt = now + (status == SharedMemory::Status::AlreadyExists ? 1s : 5s);
  if (status != mLastBlockStatus) {
    mLastBlockStatus = status;
    if (status == SharedMemory::Status::AccessDenied || status == SharedMemory::Status::AlreadyExists) {
      Log::Error(
        "Shared memory {}: another program holds it, usually a game running as administrator. "
        "If a game runs as administrator, restart it normally. Retrying",
        SharedMemory::StatusName(status));
    } else {
      Log::Error("Shared memory couldn't be created ({}); retrying every 5 s", SharedMemory::StatusName(status));
    }
  }
}

void RenderLoop::Impl::FollowLayer() {
  if (!mPublisher) {
    return;
  }
  const auto status = mPublisher->ReadStatus();
  if (!status || status->mLayerProcessID == 0) {
    return;
  }
  {
    std::scoped_lock lock(mStatsMutex);
    if (mStats.mLayerProcessID != status->mLayerProcessID) {
      Log::Info(
        "Layer attached: {} (process {}), runtime '{}'",
        std::string {status->mExecutable, strnlen(status->mExecutable, sizeof(status->mExecutable))},
        status->mLayerProcessID,
        std::string {status->mRuntime, strnlen(status->mRuntime, sizeof(status->mRuntime))});
    }
    mStats.mLayerProcessID = status->mLayerProcessID;
  }
  // The game's adapter: shared textures only open on the same GPU
  const LUID adapter {status->mAdapterLuidLow, status->mAdapterLuidHigh};
  if ((adapter.LowPart || adapter.HighPart) && mGpu && !Render::SameLuid(mGpu->AdapterLuid(), adapter)) {
    Log::Info("The game uses another GPU; moving panels to it");
    EnsureGpu(adapter);
  }
  // The layer's panel format family
  if (status->mPreferredPanelFormat != 0) {
    const auto format = RingFormatFor(status->mPreferredPanelFormat);
    if (format != mRingFormat) {
      Log::Info("Panel textures follow the game's format: DXGI {}", static_cast<int>(format));
      mRingFormat = format;
      for (auto& panel: mPanels) {
        panel.mRing.reset();
        panel.mDirty.Mark(Panels::Dirty::Structural);
      }
    }
  }
}

SharedAbi::PanelEntry RenderLoop::Impl::EntryFor(const HostPanel& panel, bool visible) const {
  SharedAbi::PanelEntry entry {};
  const auto& definition = panel.mDefinition;
  entry.mPanelID = definition.mID;
  const bool ready = panel.mRing && panel.mContentVersion > 0;
  entry.mFlags = (visible && panel.mVisible && ready) ? SharedAbi::PanelFlags::Visible : 0;
  if (!ready) {
    return entry;
  }
  const auto& ring = *panel.mRing;
  entry.mCurrentSlot = ring.NewestSlot();
  entry.mContentVersion = panel.mContentVersion;
  entry.mFenceValue = ring.FenceValue();
  float orientation[4];
  PanelOrientation(definition, orientation);
  for (int i = 0; i < 3; ++i) {
    entry.mPose[i] = definition.mPosition[i];
  }
  for (int i = 0; i < 4; ++i) {
    entry.mPose[3 + i] = orientation[i];
  }
  const auto size = panel.mSource->Size();
  entry.mSizeMeters[0] = definition.mWidthMeters;
  entry.mSizeMeters[1] = definition.mWidthMeters * static_cast<float>(size.mHeight) / static_cast<float>(size.mWidth);
  entry.mTextureWidth = ring.Width();
  entry.mTextureHeight = ring.Height();
  entry.mContentWidth = size.mWidth;
  entry.mContentHeight = size.mHeight;
  entry.mTextureGeneration = panel.mTextureGeneration;
  entry.mTextureFormat = static_cast<uint32_t>(ring.Format());
  for (uint32_t slot = 0; slot < SharedAbi::RingSlots; ++slot) {
    entry.mTextureHandles[slot] = ring.TextureHandle(slot);
  }
  entry.mFenceHandle = ring.FenceHandle();
  return entry;
}

void RenderLoop::Impl::Publish(bool visible) {
  if (!mPublisher) {
    return;
  }
  std::array<SharedAbi::PanelEntry, SharedAbi::MaxPanels> entries {};
  const auto count = std::min<std::size_t>(mPanels.size(), entries.size());
  for (std::size_t i = 0; i < count; ++i) {
    entries[i] = EntryFor(mPanels[i], visible);
  }
  mPublisher->PublishPanels({entries.data(), count});
  std::scoped_lock lock(mStatsMutex);
  ++mStats.mPublishes;
}

bool RenderLoop::Impl::RenderPanel(HostPanel& panel, TimeUtil::TimePoint now) {
  const auto size = panel.mSource->Size();
  if (size.mWidth == 0 || size.mHeight == 0) {
    return false;
  }
  if (!panel.mRing || panel.mRing->Width() != size.mWidth || panel.mRing->Height() != size.mHeight
      || panel.mRing->Format() != mRingFormat) {
    panel.mRing = Render::PanelRing::Create(*mGpu, size.mWidth, size.mHeight, mRingFormat);
    if (!panel.mRing) {
      return false;
    }
    ++panel.mTextureGeneration;
  }
  auto& ring = *panel.mRing;
  const auto slot = ring.NextSlot();
  auto* d2d = mGpu->D2D();
  d2d->SetTarget(ring.Target(slot));
  d2d->BeginDraw();
  panel.mSource->Render(d2d, size, now);
  const auto drawn = d2d->EndDraw();
  d2d->SetTarget(nullptr);
  if (FAILED(drawn)) {
    Log::Error("Panel '{}' failed to draw (0x{:08X})", StringUtil::Narrow(panel.mDefinition.mName), static_cast<uint32_t>(drawn));
    if (drawn == D2DERR_RECREATE_TARGET) {
      mGpu.reset();// re-created on the next frame
    }
    return false;
  }

  if (mOptions.mFenceDelay > 0ms) {
    // Test only: the frame is announced before its GPU work is signalled
    panel.mContentVersion += 1;
    const auto announced = ring.NextFenceValue();
    auto entry = EntryFor(panel, true);
    entry.mCurrentSlot = slot;
    entry.mFenceValue = announced;
    std::array<SharedAbi::PanelEntry, SharedAbi::MaxPanels> entries {};
    for (std::size_t i = 0; i < mPanels.size() && i < entries.size(); ++i) {
      entries[i] = (&mPanels[i] == &panel) ? entry : EntryFor(mPanels[i], mLastVisible);
    }
    mPublisher->PublishPanels({entries.data(), std::min(mPanels.size(), entries.size())});
    Sleep(static_cast<DWORD>(mOptions.mFenceDelay.count()));
    ring.Finish(mGpu->Context(), slot);
  } else {
    ring.Finish(mGpu->Context(), slot);// signal + flush (SY-3)
    panel.mContentVersion += 1;
  }
  panel.mLimiter.Rendered(now);
  panel.mLastPublish = now;
  ++panel.mRenders;
  return true;
}

void RenderLoop::Impl::WriteStatsLine() {
  std::string perPanel;
  for (const auto& panel: mPanels) {
    perPanel += std::format(" '{}' {}", StringUtil::Narrow(panel.mDefinition.mName), panel.mRenders);
  }
  RenderStats stats;
  {
    std::scoped_lock lock(mStatsMutex);
    stats = mStats;
  }
  Log::Info(
    "Host stats (60 s): {} frames, {} panel renders ({:.1f}/s); total renders per panel:{}; layer process {}",
    mFramesSinceStats,
    mRendersSinceStats,
    static_cast<double>(mRendersSinceStats) / 60.0,
    perPanel,
    stats.mLayerProcessID);
  mFramesSinceStats = 0;
  mRendersSinceStats = 0;
}

void RenderLoop::Impl::Main(const std::atomic<bool>& visible) {
  FrameWakeup::SetTimer(mTimer);
  // HostPanel holds atomics (not movable): build the vector at its final size
  mPanels = std::vector<HostPanel>(mDefinitions.size());
  for (std::size_t i = 0; i < mDefinitions.size(); ++i) {
    mPanels[i].mDefinition = mDefinitions[i].first;
    mPanels[i].mLimiter.SetRate(mDefinitions[i].first.mMaxUpdatesPerSecond, mOptions.mGlobalMaxUpdatesPerSecond);
  }
  EnsurePublisher(TimeUtil::Now());
  std::optional<LUID> adapter;
  if (mPublisher) {
    if (const auto status = mPublisher->ReadStatus(); status && status->mLayerProcessID != 0) {
      adapter = LUID {status->mAdapterLuidLow, status->mAdapterLuidHigh};
    }
  }
  EnsureGpu(adapter);
  FrameWakeup::RequestFrameSoon();

  const HANDLE handles[] {mStop, mTimer};
  for (;;) {
    // The 1 s timeout keeps the layer report and shared-memory retries going
    const auto woke = WaitForMultipleObjects(2, handles, FALSE, 1000);
    if (woke == WAIT_OBJECT_0) {
      break;
    }
    FrameWakeup::FrameStarted();
    const auto now = TimeUtil::Now();
    ++mFramesSinceStats;
    {
      std::scoped_lock lock(mStatsMutex);
      ++mStats.mFrames;
    }

    EnsurePublisher(now);
    if (mStatusTimer.Due(now)) {
      FollowLayer();
    }
    if (!mGpu && !EnsureGpu(std::nullopt)) {
      continue;
    }

    const bool showing = visible.load();
    bool publish = showing != mLastVisible;
    if (showing && !mLastVisible) {
      for (auto& panel: mPanels) {
        panel.mDirty.Mark(Panels::Dirty::Structural);
      }
    }
    mLastVisible = showing;

    for (auto& panel: mPanels) {
      if (!mGpu) {
        break;// lost mid-frame; re-created next frame
      }
      if (!showing || !panel.mVisible || !panel.mSource || !panel.mDirty.IsSet()) {
        continue;// hidden means idle; the request waits until shown
      }
      const auto kind = panel.mDirty.Take();
      const bool structural = kind & Panels::Dirty::Structural;
      const auto floor = panel.mLastPublish + mOptions.mRingSafetyFloor;
      if (!panel.mLimiter.MayRender(now, structural) || (panel.mContentVersion > 0 && now < floor)) {
        panel.mDirty.Restore(kind);// postponed, never dropped
        FrameWakeup::ScheduleNextFrame(std::max(panel.mLimiter.NextAllowed(), floor));
        continue;
      }
      if (RenderPanel(panel, now)) {
        publish = true;
        ++mRendersSinceStats;
        std::scoped_lock lock(mStatsMutex);
        ++mStats.mRenders;
      } else if (mGpu) {
        panel.mDirty.Restore(kind);
      }
      if (panel.mSource && panel.mSource->IsAnimated()) {
        panel.mDirty.Mark(Panels::Dirty::Content);
        FrameWakeup::ScheduleNextFrame(panel.mLimiter.NextAllowed());
      }
    }
    if (publish && mOptions.mFenceDelay == 0ms) {
      Publish(showing);
    } else if (publish && !showing) {
      Publish(false);
    }
    if (!mGpu) {
      FrameWakeup::RequestFrameSoon();
    }
    if (mStatsTimer.Due(now)) {
      WriteStatsLine();
    }
  }
  FrameWakeup::SetTimer(nullptr);
}

}// namespace OpenDesktopVR::Host
