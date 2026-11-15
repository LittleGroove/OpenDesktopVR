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

#include <winrt/base.h>

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
  SourceFactory mFactory;
  std::vector<std::unique_ptr<HostPanel>> mPanels;
  std::unique_ptr<Render::GpuDevice> mGpu;
  std::optional<Publisher> mPublisher;
  DXGI_FORMAT mRingFormat {DXGI_FORMAT_B8G8R8A8_UNORM};
  bool mLastVisible {true};
  bool mLastViewers {false};

  std::mutex mPendingMutex;
  std::optional<std::vector<PanelDefinition>> mPendingPanels;

  std::thread mThread;
  HANDLE mStop {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE mTimer {CreateWaitableTimerW(nullptr, FALSE, nullptr)};

  mutable std::mutex mStatsMutex;
  RenderStats mStats;
  TimeUtil::PeriodicTimer mStatsTimer {60s};
  TimeUtil::PeriodicTimer mSecondTimer {1s};
  TimeUtil::TimePoint mNextPublisherAttempt {};
  SharedMemory::Status mLastBlockStatus {SharedMemory::Status::Ok};
  uint64_t mRendersSinceStats {0};
  uint64_t mFramesSinceStats {0};

  // Test only (late-fence mode): signals waiting to be issued, oldest first
  struct PendingSignal {
    TimeUtil::TimePoint mDue;
    HostPanel* mPanel;
    uint32_t mTextureGeneration;
    uint64_t mValue;
  };
  static constexpr std::size_t MaxPendingSignals = 64;
  std::vector<PendingSignal> mPendingSignals;

  ~Impl() {
    CloseHandle(mStop);
    CloseHandle(mTimer);
  }

  void Main(const std::atomic<bool>& visible);
  void ApplyPendingPanels();
  void MakeSource(HostPanel& panel);
  bool EnsureGpu(std::optional<LUID> adapter);
  void EnsurePublisher(TimeUtil::TimePoint now);
  void FollowLayer();
  bool RenderPanel(HostPanel& panel, TimeUtil::TimePoint now);
  void Publish(bool visible);
  SharedAbi::PanelEntry EntryFor(const HostPanel& panel, bool visible) const;
  void IssueDueSignals(TimeUtil::TimePoint now);
  void WriteStatsLine();
};

RenderLoop::RenderLoop(RenderLoopOptions options, SourceFactory factory) : mImpl(std::make_unique<Impl>()) {
  mImpl->mOptions = options;
  mImpl->mFactory = std::move(factory);
}

RenderLoop::~RenderLoop() {
  Stop();
}

void RenderLoop::SetPanels(std::vector<PanelDefinition> panels) {
  {
    std::scoped_lock lock(mImpl->mPendingMutex);
    mImpl->mPendingPanels = std::move(panels);
  }
  FrameWakeup::RequestFrameSoon();
}

void RenderLoop::Start() {
  if (mImpl->mThread.joinable()) {
    return;
  }
  mImpl->mThread = std::thread([this] {
    ThreadUtil::SetCurrentThreadName(L"Panel renderer");
    ThreadUtil::CatchAll("panel render thread", [this] {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);// capture uses WinRT
      mImpl->Main(mPanelsVisible);
    });
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

void RenderLoop::Impl::MakeSource(HostPanel& panel) {
  panel.mSource.reset();
  panel.mRing.reset();
  panel.mActive = false;
  if (!mGpu || !mFactory) {
    return;
  }
  // The callback may run on a capture thread after the panel is gone: it
  // holds the dirty flag, never the panel
  std::weak_ptr<Panels::DirtyFlag> dirty = panel.mDirty;
  panel.mSource = mFactory(panel.mDefinition, *mGpu, [dirty] {
    if (auto flag = dirty.lock()) {
      flag->Mark(Panels::Dirty::Content);
      FrameWakeup::RequestFrameSoon();// at most every 8 ms (CP-2)
    }
  });
  panel.mDirty->Mark(Panels::Dirty::Structural);
}

void RenderLoop::Impl::ApplyPendingPanels() {
  std::optional<std::vector<PanelDefinition>> pending;
  {
    std::scoped_lock lock(mPendingMutex);
    pending = std::exchange(mPendingPanels, std::nullopt);
  }
  if (!pending) {
    return;
  }
  std::vector<std::unique_ptr<HostPanel>> next;
  for (auto& definition: *pending) {
    if (next.size() >= SharedAbi::MaxPanels) {
      Log::Warning("More than {} panels: the rest aren't shown", SharedAbi::MaxPanels);
      break;
    }
    std::unique_ptr<HostPanel> panel;
    for (auto& existing: mPanels) {
      if (existing && existing->mDefinition.mID == definition.mID) {
        panel = std::move(existing);
        break;
      }
    }
    const bool newContent = !panel || !panel->mDefinition.SameContent(definition);
    if (!panel) {
      panel = std::make_unique<HostPanel>();
    }
    panel->mDefinition = std::move(definition);
    panel->mLimiter.SetRate(panel->mDefinition.mMaxUpdatesPerSecond, mOptions.mGlobalMaxUpdatesPerSecond);
    if (newContent) {
      MakeSource(*panel);
    }
    panel->mDirty->Mark(Panels::Dirty::Structural);
    next.push_back(std::move(panel));
  }
  // Removed panels (and their sources) go here
  mPendingSignals.clear();
  mPanels = std::move(next);
  Publish(mLastVisible);
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
    panel->mSource.reset();
    panel->mRing.reset();
  }
  mPendingSignals.clear();
  mGpu = std::move(gpu);
  for (auto& panel: mPanels) {
    MakeSource(*panel);
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
      panel->mDirty->Mark(Panels::Dirty::Structural);
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
        "Shared memory {}: another program holds it. If a game runs as administrator, restart it normally. Retrying",
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
        panel->mRing.reset();
        panel->mDirty->Mark(Panels::Dirty::Structural);
      }
    }
  }
}

SharedAbi::PanelEntry RenderLoop::Impl::EntryFor(const HostPanel& panel, bool visible) const {
  SharedAbi::PanelEntry entry {};
  const auto& definition = panel.mDefinition;
  entry.mPanelID = definition.mID;
  const bool ready = panel.mRing && panel.mSource && panel.mContentVersion > 0;
  entry.mFlags = (visible && ready) ? SharedAbi::PanelFlags::Visible : 0;
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
  entry.mSizeMeters[0] = definition.mWidthMeters;
  entry.mSizeMeters[1] = definition.mWidthMeters * static_cast<float>(ring.Height()) / static_cast<float>(ring.Width());
  entry.mTextureWidth = ring.Width();
  entry.mTextureHeight = ring.Height();
  entry.mContentWidth = ring.Width();
  entry.mContentHeight = ring.Height();
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
    entries[i] = EntryFor(*mPanels[i], visible);
  }
  mPublisher->PublishPanels({entries.data(), count});
  std::scoped_lock lock(mStatsMutex);
  ++mStats.mPublishes;
}

bool RenderLoop::Impl::RenderPanel(HostPanel& panel, TimeUtil::TimePoint now) {
  const auto size = panel.mSource->Size();
  if (size.mWidth == 0 || size.mHeight == 0 || size.mWidth > 16384 || size.mHeight > 16384) {
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
    // Test only: announce the frame now, signal its fence later, while
    // rendering carries on, like a host whose GPU work is starved
    const auto value = ring.Advance(slot);
    panel.mContentVersion += 1;
    if (mPendingSignals.size() < MaxPendingSignals) {
      mPendingSignals.push_back({now + mOptions.mFenceDelay, &panel, panel.mTextureGeneration, value});
      FrameWakeup::ScheduleNextFrame(now + mOptions.mFenceDelay);
    }
    // Full: this signal is dropped (the queue never grows). A later, higher
    // signal still completes anything waiting on this value.
  } else {
    ring.Finish(mGpu->Context(), slot);// signal + flush (SY-3)
    panel.mContentVersion += 1;
  }
  panel.mLimiter.Rendered(now);
  panel.mLastPublish = now;
  ++panel.mRenders;
  return true;
}

void RenderLoop::Impl::IssueDueSignals(TimeUtil::TimePoint now) {
  std::size_t issued = 0;
  for (const auto& pending: mPendingSignals) {
    if (pending.mDue > now) {
      FrameWakeup::ScheduleNextFrame(pending.mDue);
      break;
    }
    auto& panel = *pending.mPanel;
    if (mGpu && panel.mRing && panel.mTextureGeneration == pending.mTextureGeneration) {
      panel.mRing->Signal(mGpu->Context(), pending.mValue);
    }
    ++issued;
  }
  mPendingSignals.erase(mPendingSignals.begin(), mPendingSignals.begin() + static_cast<std::ptrdiff_t>(issued));
}

void RenderLoop::Impl::WriteStatsLine() {
  std::string perPanel;
  for (const auto& panel: mPanels) {
    perPanel += std::format(" '{}' {}", StringUtil::Narrow(panel->mDefinition.mName), panel->mRenders);
  }
  RenderStats stats;
  {
    std::scoped_lock lock(mStatsMutex);
    stats = mStats;
  }
  Log::Info(
    "Host stats (60 s): {} frames, {} panel renders ({:.1f}/s); total renders per panel:{}; viewers {}; layer process {}",
    mFramesSinceStats,
    mRendersSinceStats,
    static_cast<double>(mRendersSinceStats) / 60.0,
    perPanel,
    stats.mHasViewers ? "yes" : "none",
    stats.mLayerProcessID);
  mFramesSinceStats = 0;
  mRendersSinceStats = 0;
}

void RenderLoop::Impl::Main(const std::atomic<bool>& visible) {
  FrameWakeup::SetTimer(mTimer);
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
    // The 1 s timeout keeps the layer report, window re-finding and
    // shared-memory retries going
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
    ApplyPendingPanels();
    IssueDueSignals(now);
    if (mSecondTimer.Due(now)) {
      FollowLayer();
      for (auto& panel: mPanels) {
        if (panel->mSource) {
          ThreadUtil::CatchAll("source maintenance", [&] { panel->mSource->Maintain(now); });
        }
      }
    }
    if (!mGpu && !EnsureGpu(std::nullopt)) {
      continue;
    }

    // Nobody watching: draw nothing and let sources stop producing frames
    const bool viewers = mOptions.mRenderWithoutViewers || (mPublisher && mPublisher->HasViewers());
    if (viewers != mLastViewers) {
      Log::Info("{}", viewers ? "A viewer is watching: drawing panels" : "Nobody is watching: panels paused");
      mLastViewers = viewers;
      std::scoped_lock lock(mStatsMutex);
      mStats.mHasViewers = viewers;
    }
    const bool showing = visible.load();
    bool publish = showing != mLastVisible;
    mLastVisible = showing;
    for (auto& panel: mPanels) {
      const bool active = showing && viewers && panel->mSource;
      if (active != panel->mActive) {
        panel->mActive = active;
        if (panel->mSource) {
          panel->mSource->SetActive(active);// hidden or unwatched: idle (CP-5)
        }
        if (active) {
          panel->mDirty->Mark(Panels::Dirty::Structural);
        }
      }
    }
    if (!viewers) {
      if (publish) {
        Publish(showing);
      }
      continue;
    }

    for (auto& owned: mPanels) {
      auto& panel = *owned;
      if (!mGpu) {
        break;// lost mid-frame; re-created next frame
      }
      if (!panel.mActive || !panel.mDirty->IsSet()) {
        continue;// hidden means idle; the request waits until shown
      }
      const auto kind = panel.mDirty->Take();
      const bool structural = kind & Panels::Dirty::Structural;
      const auto floor = panel.mLastPublish + mOptions.mRingSafetyFloor;
      if (!panel.mLimiter.MayRender(now, structural) || (panel.mContentVersion > 0 && now < floor)) {
        panel.mDirty->Restore(kind);// postponed, never dropped
        FrameWakeup::ScheduleNextFrame(std::max(panel.mLimiter.NextAllowed(), floor));
        continue;
      }
      bool rendered = false;
      ThreadUtil::CatchAll("panel render", [&] { rendered = RenderPanel(panel, now); });
      if (rendered) {
        publish = true;
        ++mRendersSinceStats;
        std::scoped_lock lock(mStatsMutex);
        ++mStats.mRenders;
      } else if (mGpu) {
        panel.mDirty->Restore(kind & Panels::Dirty::Content);
      }
      if (panel.mSource && panel.mSource->IsAnimated()) {
        panel.mDirty->Mark(Panels::Dirty::Content);
        FrameWakeup::ScheduleNextFrame(panel.mLimiter.NextAllowed());
      }
    }
    if (publish) {
      Publish(showing);
    }
    if (!mGpu) {
      FrameWakeup::RequestFrameSoon();
    }
    if (mStatsTimer.Due(now)) {
      WriteStatsLine();
    }
  }
  // Sources (capture sessions) stop before the GPU goes
  mPanels.clear();
  FrameWakeup::SetTimer(nullptr);
}

}// namespace OpenDesktopVR::Host
