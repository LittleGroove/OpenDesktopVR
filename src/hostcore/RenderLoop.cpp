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

#include <d2d1effects.h>

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

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
  bool mLastViewers {false};
  // The panel the layer says the head points at (gaze focus)
  uint64_t mFocusedPanel {0};
  // Applies a panel's look (brightness, tint, opacity) as it's drawn
  winrt::com_ptr<ID2D1Effect> mLookEffect;

  std::mutex mPendingMutex;
  std::optional<std::vector<PanelDefinition>> mPendingPanels;
  std::set<uint64_t> mPendingHidden;

  std::thread mThread;
  HANDLE mStop {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  // Set by a layer when the focused panel changes (shared through the block)
  HANDLE mLayerWake {CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  // Set by a layer after each tracking sample; the hand thread waits on it
  HANDLE mTrackingEvent {CreateEventW(nullptr, FALSE, FALSE, nullptr)};

  // The block, for the hand thread, once it exists (it then lives as long
  // as the render loop)
  std::atomic<Publisher*> mSharedPublisher {nullptr};
  struct Channel final : TrackingChannel {
    Impl& mImpl;
    std::mutex mMutex;
    SharedAbi::HandState mLast {};
    explicit Channel(Impl& impl) : mImpl(impl) {
    }
    void* Event() const noexcept override {
      return mImpl.mTrackingEvent;
    }
    uint64_t Written() const noexcept override {
      const auto* publisher = mImpl.mSharedPublisher.load(std::memory_order_acquire);
      return publisher ? publisher->TrackingWritten() : 0;
    }
    bool Read(uint64_t index, SharedAbi::TrackingSample& out) const noexcept override {
      const auto* publisher = mImpl.mSharedPublisher.load(std::memory_order_acquire);
      return publisher && publisher->ReadTrackingSample(index, out);
    }
    void WriteHandState(const SharedAbi::HandState& state) noexcept override {
      std::scoped_lock lock(mMutex);
      mLast = state;
      if (auto* publisher = mImpl.mSharedPublisher.load(std::memory_order_acquire)) {
        publisher->WriteHandState(state);
      }
    }
    // The render thread, when the block is created
    void Rewrite(Publisher& publisher) noexcept {
      std::scoped_lock lock(mMutex);
      publisher.WriteHandState(mLast);
    }
  } mChannel {*this};
  std::atomic<bool> mHandsEnabled {true};
  bool mHandsApplied {true};
  // "Recenter": asked for (any thread), and as last written to the block
  std::atomic<uint64_t> mRecenterCount {0};
  std::optional<uint64_t> mRecenterWritten;
  mutable std::mutex mTouchMutex;
  std::shared_ptr<const TouchPanels> mTouchPanels {std::make_shared<const TouchPanels>()};
  bool mHighResolutionTimer {false};
  HANDLE mTimer {FrameWakeup::CreateTimer(&mHighResolutionTimer)};

  // Each panel's content size before trimming, for the settings window;
  // written on the render thread when it changes
  mutable std::mutex mSizesMutex;
  std::array<std::pair<uint64_t, Render::PixelSize>, SharedAbi::MaxPanels> mUntrimmedSizes {};

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
    CloseHandle(mLayerWake);
    CloseHandle(mTrackingEvent);
  }

  void Main();
  void FollowFocus();
  bool PrepareLook(HostPanel& panel, D2D1_SIZE_U capacity, Render::PixelSize size, TimeUtil::TimePoint now);
  void DrawLook(HostPanel& panel);
  void ApplyPendingPanels();
  void MakeSource(HostPanel& panel);
  // The hands may touch it: its app gets ready for input at capture start
  bool WantsInput(const PanelDefinition& definition) const {
    return mHandsEnabled.load(std::memory_order_relaxed) && (definition.mTouch || definition.mScroll);
  }
  void FollowHandsEnabled();
  bool EnsureGpu(std::optional<LUID> adapter);
  void EnsurePublisher(TimeUtil::TimePoint now);
  void FollowLayer();
  bool RenderPanel(HostPanel& panel, TimeUtil::TimePoint now);
  void Publish();
  SharedAbi::PanelEntry EntryFor(const HostPanel& panel) const;
  void IssueDueSignals(TimeUtil::TimePoint now);
  void WriteStatsLine();
  void UpdateUntrimmedSizes();
};

RenderLoop::RenderLoop(RenderLoopOptions options, SourceFactory factory) : mImpl(std::make_unique<Impl>()) {
  mImpl->mOptions = options;
  mImpl->mFactory = std::move(factory);
}

RenderLoop::~RenderLoop() {
  Stop();
}

void RenderLoop::Recenter() {
  mImpl->mRecenterCount.fetch_add(1);
  FrameWakeup::RequestFrameSoon();
}

void RenderLoop::SetHandsEnabled(bool enabled) {
  mImpl->mHandsEnabled = enabled;
  FrameWakeup::RequestFrameSoon();
}

// Render thread: hands turned on or off since the last frame
void RenderLoop::Impl::FollowHandsEnabled() {
  const bool enabled = mHandsEnabled.load(std::memory_order_relaxed);
  if (enabled == mHandsApplied) {
    return;
  }
  mHandsApplied = enabled;
  for (auto& panel: mPanels) {
    if (panel->mSource) {
      panel->mSource->SetInputWanted(WantsInput(panel->mDefinition));
    }
  }
}

TrackingChannel& RenderLoop::Tracking() noexcept {
  return mImpl->mChannel;
}

std::shared_ptr<const TouchPanels> RenderLoop::TouchablePanels() const {
  std::scoped_lock lock(mImpl->mTouchMutex);
  return mImpl->mTouchPanels;
}

void RenderLoop::SetPanels(std::vector<PanelDefinition> panels, std::set<uint64_t> hidden) {
  {
    std::scoped_lock lock(mImpl->mPendingMutex);
    mImpl->mPendingPanels = std::move(panels);
    mImpl->mPendingHidden = std::move(hidden);
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
      mImpl->Main();
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

RenderStats RenderLoop::Stats() const noexcept {
  std::scoped_lock lock(mImpl->mStatsMutex);
  return mImpl->mStats;
}

std::optional<Render::PixelSize> RenderLoop::UntrimmedSize(uint64_t panelID) const noexcept {
  std::scoped_lock lock(mImpl->mSizesMutex);
  for (const auto& [id, size]: mImpl->mUntrimmedSizes) {
    if (id == panelID && size.mWidth > 0 && size.mHeight > 0) {
      return size;
    }
  }
  return std::nullopt;
}

void RenderLoop::Impl::UpdateUntrimmedSizes() {
  decltype(mUntrimmedSizes) sizes {};
  for (std::size_t i = 0; i < mPanels.size() && i < sizes.size(); ++i) {
    const auto& panel = *mPanels[i];
    sizes[i] = {panel.mDefinition.mID, panel.mSource ? panel.mSource->UntrimmedSize() : Render::PixelSize {}};
  }
  std::scoped_lock lock(mSizesMutex);
  mUntrimmedSizes = sizes;
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
  if (panel.mSource) {
    panel.mSource->SetCrop(panel.mDefinition.mCrop);
    panel.mSource->SetKeepUpdatingWhenCovered(panel.mDefinition.mKeepUpdatingWhenCovered);
    panel.mSource->SetInputWanted(WantsInput(panel.mDefinition));
  }
  panel.mDirty->Mark(Panels::Dirty::Structural);
}

void RenderLoop::Impl::ApplyPendingPanels() {
  std::optional<std::vector<PanelDefinition>> pending;
  std::set<uint64_t> hidden;
  {
    std::scoped_lock lock(mPendingMutex);
    pending = std::exchange(mPendingPanels, std::nullopt);
    hidden = mPendingHidden;
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
    // ST-6: an edit touches only what it changes. New content gets a new
    // source; a new crop redraws; placement and rate changes only publish.
    const bool newContent = !panel || !panel->mDefinition.SameContent(definition);
    const bool newCrop = panel && !(panel->mDefinition.mCrop == definition.mCrop);
    const bool newLook = panel && !panel->mDefinition.SameLook(definition);
    const bool newKeepUpdating = panel && panel->mDefinition.mKeepUpdatingWhenCovered != definition.mKeepUpdatingWhenCovered;
    const bool newInput = panel && WantsInput(panel->mDefinition) != WantsInput(definition);
    if (!panel) {
      panel = std::make_unique<HostPanel>();
    }
    panel->mDefinition = std::move(definition);
    if (newKeepUpdating && !newContent && panel->mSource) {
      panel->mSource->SetKeepUpdatingWhenCovered(panel->mDefinition.mKeepUpdatingWhenCovered);
    }
    if (newInput && !newContent && panel->mSource) {
      panel->mSource->SetInputWanted(WantsInput(panel->mDefinition));
    }
    panel->mLimiter.SetRate(panel->mDefinition.mMaxUpdatesPerSecond, mOptions.mGlobalMaxUpdatesPerSecond);
    if (newContent) {
      MakeSource(*panel);
    } else if (newCrop && panel->mSource) {
      panel->mSource->SetCrop(panel->mDefinition.mCrop);
      panel->mDirty->Mark(Panels::Dirty::Structural);
    } else if (newLook) {
      panel->mDirty->Mark(Panels::Dirty::Structural);
    }
    panel->mHidden = hidden.contains(panel->mDefinition.mID);
    next.push_back(std::move(panel));
  }
  // Removed panels (and their sources) go here
  mPendingSignals.clear();
  mPanels = std::move(next);
  Publish();
}

void RenderLoop::Impl::FollowFocus() {
  if (!mPublisher) {
    return;
  }
  const auto status = mPublisher->ReadStatus();
  const uint64_t focused = (status && status->mLayerProcessID != 0) ? status->mFocusedPanelID : 0;
  if (focused == mFocusedPanel) {
    return;
  }
  // Redraw the panels whose look depends on it: the old and the new
  for (auto& panel: mPanels) {
    const auto id = panel->mDefinition.mID;
    if ((id == focused || id == mFocusedPanel) && panel->mDefinition.mOpacity != panel->mDefinition.mFocusedOpacity) {
      panel->mDirty->Mark(Panels::Dirty::Structural);
    }
  }
  mFocusedPanel = focused;
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
  mPublisher = Publisher::Create(status, mLayerWake, mTrackingEvent);
  if (mPublisher) {
    Log::Info("Shared memory created; layers can attach");
    mSharedPublisher.store(&*mPublisher, std::memory_order_release);
    mRecenterWritten.reset();// a new block: written again
    mChannel.Rewrite(*mPublisher);
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
    mStats.mLayerExecutable = StringUtil::Widen(std::string {status->mExecutable, strnlen(status->mExecutable, sizeof(status->mExecutable))});
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

SharedAbi::PanelEntry RenderLoop::Impl::EntryFor(const HostPanel& panel) const {
  SharedAbi::PanelEntry entry {};
  const auto& definition = panel.mDefinition;
  entry.mPanelID = definition.mID;
  const bool ready = panel.mRing && panel.mSource && panel.mContentVersion > 0;
  entry.mFlags = (!panel.mHidden && !panel.mSourceHidden && ready) ? SharedAbi::PanelFlags::Visible : 0;
  if (definition.mID == mFocusedPanel) {
    entry.mFlags |= SharedAbi::PanelFlags::Focused;
  }
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
  const auto content = ring.ContentSize(ring.NewestSlot());
  if (content.mWidth == 0 || content.mHeight == 0) {
    entry.mFlags = 0;// nothing drawn yet
    return entry;
  }
  entry.mSizeMeters[0] = definition.mWidthMeters;
  entry.mSizeMeters[1] = definition.mWidthMeters * static_cast<float>(content.mHeight) / static_cast<float>(content.mWidth);
  entry.mTextureWidth = ring.Width();
  entry.mTextureHeight = ring.Height();
  entry.mContentWidth = content.mWidth;
  entry.mContentHeight = content.mHeight;
  for (uint32_t slot = 0; slot < SharedAbi::RingSlots; ++slot) {
    const auto slotContent = ring.ContentSize(slot);
    entry.mSlotContentSize[slot][0] = slotContent.mWidth;
    entry.mSlotContentSize[slot][1] = slotContent.mHeight;
  }
  entry.mTextureGeneration = panel.mTextureGeneration;
  entry.mTextureFormat = static_cast<uint32_t>(ring.Format());
  for (uint32_t slot = 0; slot < SharedAbi::RingSlots; ++slot) {
    entry.mTextureHandles[slot] = ring.TextureHandle(slot);
  }
  entry.mFenceHandle = ring.FenceHandle();
  return entry;
}

void RenderLoop::Impl::Publish() {
  if (!mPublisher) {
    return;
  }
  std::array<SharedAbi::PanelEntry, SharedAbi::MaxPanels> entries {};
  const auto count = std::min<std::size_t>(mPanels.size(), entries.size());
  for (std::size_t i = 0; i < count; ++i) {
    entries[i] = EntryFor(*mPanels[i]);
  }
  mPublisher->PublishPanels({entries.data(), count});
  {
    std::scoped_lock lock(mStatsMutex);
    ++mStats.mPublishes;
  }
  // The panels the hands may touch: what the layer now shows
  TouchPanels touchable;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& entry = entries[i];
    if (!(entry.mFlags & SharedAbi::PanelFlags::Visible)) {
      continue;
    }
    const auto& definition = mPanels[i]->mDefinition;
    TouchPanel panel {.mID = definition.mID, .mName = definition.mName, .mTouch = definition.mTouch, .mScroll = definition.mScroll,
      .mKeyboard = definition.mKind == PanelKind::Keyboard};
    if (mPanels[i]->mSource) {
      panel.mTarget = mPanels[i]->mSource->Input();
    }
    panel.mGeometry = {
      .mID = definition.mID,
      .mPosition = {entry.mPose[0], entry.mPose[1], entry.mPose[2]},
      .mOrientation = {entry.mPose[3], entry.mPose[4], entry.mPose[5], entry.mPose[6]},
      .mSize = {entry.mSizeMeters[0], entry.mSizeMeters[1]},
    };
    touchable.push_back(std::move(panel));
  }
  std::scoped_lock lock(mTouchMutex);
  if (*mTouchPanels != touchable) {
    mTouchPanels = std::make_shared<const TouchPanels>(std::move(touchable));
  }
}

// A panel's look (brightness and tint on the colours, opacity on alpha,
// premultiplied) takes two steps. First, before the ring slot is drawn: the
// content, into the panel's own bitmap. False if that can't be done (the
// plain content is drawn instead).
bool RenderLoop::Impl::PrepareLook(HostPanel& panel, D2D1_SIZE_U capacity, Render::PixelSize size, TimeUtil::TimePoint now) {
  auto* d2d = mGpu->D2D();
  if (!mLookEffect && FAILED(d2d->CreateEffect(CLSID_D2D1ColorMatrix, mLookEffect.put()))) {
    return false;
  }
  if (!panel.mLookTarget || panel.mLookTarget->GetPixelSize().width != capacity.width
      || panel.mLookTarget->GetPixelSize().height != capacity.height) {
    panel.mLookTarget = nullptr;
    const D2D1_BITMAP_PROPERTIES1 properties {
      .pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
      .dpiX = 96,
      .dpiY = 96,
      .bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET,
    };
    if (FAILED(d2d->CreateBitmap(capacity, nullptr, 0, properties, panel.mLookTarget.put()))) {
      return false;
    }
  }
  d2d->SetTarget(panel.mLookTarget.get());
  d2d->BeginDraw();
  d2d->Clear(D2D1_COLOR_F {0, 0, 0, 0});
  d2d->PushAxisAlignedClip(
    D2D1_RECT_F {0, 0, static_cast<float>(size.mWidth), static_cast<float>(size.mHeight)}, D2D1_ANTIALIAS_MODE_ALIASED);
  panel.mSource->Render(d2d, size, now);
  d2d->PopAxisAlignedClip();
  const bool drawn = SUCCEEDED(d2d->EndDraw());
  d2d->SetTarget(nullptr);
  return drawn;
}

// Then, inside the ring slot's draw: that bitmap through the matrix,
// replacing what's there
void RenderLoop::Impl::DrawLook(HostPanel& panel) {
  auto* d2d = mGpu->D2D();
  const auto multipliers = LookMultipliers(panel.mDefinition, panel.mDefinition.mID == mFocusedPanel);
  const D2D1_MATRIX_5X4_F matrix {
    multipliers[0], 0, 0, 0,
    0, multipliers[1], 0, 0,
    0, 0, multipliers[2], 0,
    0, 0, 0, multipliers[3],
    0, 0, 0, 0,
  };
  mLookEffect->SetInput(0, panel.mLookTarget.get());
  mLookEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix);
  mLookEffect->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED);
  d2d->DrawImage(mLookEffect.get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
}

bool RenderLoop::Impl::RenderPanel(HostPanel& panel, TimeUtil::TimePoint now) {
  const auto size = panel.mSource->Size();
  if (size.mWidth == 0 || size.mHeight == 0 || size.mWidth > 16384 || size.mHeight > 16384) {
    return false;
  }
  // Content showing part of another panel (the keyboard's preview strip)
  // draws from that panel's newest frame, on this same device
  struct PartGuard {
    Render::ContentSource& mSource;
    ~PartGuard() {
      mSource.SetPart(nullptr, {});
    }
  } partGuard {*panel.mSource};
  if (const auto partOf = panel.mSource->PartOfPanel()) {
    for (const auto& other: mPanels) {
      if (other->mDefinition.mID == partOf && other->mRing && other->mContentVersion > 0 && other.get() != &panel) {
        const auto newest = other->mRing->NewestSlot();
        panel.mSource->SetPart(other->mRing->Source(newest), other->mRing->ContentSize(newest));
        break;
      }
    }
  }
  // RN-7: the textures only grow. A smaller frame (a window made smaller, a
  // new trim) uses their top-left part, so nothing is re-created here or in
  // the game while you drag.
  if (!panel.mRing || !panel.mRing->Fits(size) || panel.mRing->Format() != mRingFormat) {
    const Render::PixelSize current = panel.mRing ? Render::PixelSize {panel.mRing->Width(), panel.mRing->Height()} : Render::PixelSize {};
    const auto capacity = Render::PanelRing::CapacityFor(size, current);
    panel.mRing = Render::PanelRing::Create(*mGpu, capacity.mWidth, capacity.mHeight, mRingFormat);
    if (!panel.mRing) {
      return false;
    }
    ++panel.mTextureGeneration;
  }
  auto& ring = *panel.mRing;
  const auto slot = ring.NextSlot();
  auto* d2d = mGpu->D2D();
  // Brightness, tint or opacity: the content goes through the panel's own
  // bitmap first (doc 08: the host applies the look; the layer only copies)
  const bool look = !panel.mDefinition.IsPlain(panel.mDefinition.mID == mFocusedPanel)
    && PrepareLook(panel, D2D1_SIZE_U {ring.Width(), ring.Height()}, size, now);
  d2d->SetTarget(ring.Target(slot));
  d2d->BeginDraw();
  const auto w = static_cast<float>(size.mWidth);
  const auto h = static_cast<float>(size.mHeight);
  // One transparent pixel past the content's right and bottom edges: a
  // compositor filtering at the edge blends with that, never with an older,
  // larger frame left in the texture
  const auto edgeW = static_cast<float>(std::min(size.mWidth + 1, ring.Width()));
  const auto edgeH = static_cast<float>(std::min(size.mHeight + 1, ring.Height()));
  for (const D2D1_RECT_F margin: {D2D1_RECT_F {w, 0, edgeW, edgeH}, D2D1_RECT_F {0, h, edgeW, edgeH}}) {
    if (margin.right > margin.left && margin.bottom > margin.top) {
      d2d->PushAxisAlignedClip(margin, D2D1_ANTIALIAS_MODE_ALIASED);
      d2d->Clear(D2D1_COLOR_F {0, 0, 0, 0});
      d2d->PopAxisAlignedClip();
    }
  }
  // Sources draw only within the content (their Clear too)
  d2d->PushAxisAlignedClip(D2D1_RECT_F {0, 0, w, h}, D2D1_ANTIALIAS_MODE_ALIASED);
  if (look) {
    DrawLook(panel);
  } else {
    panel.mSource->Render(d2d, size, now);
  }
  d2d->PopAxisAlignedClip();
  const auto drawn = d2d->EndDraw();
  d2d->SetTarget(nullptr);
  if (FAILED(drawn)) {
    Log::Error("Panel '{}' failed to draw (0x{:08X})", StringUtil::Narrow(panel.mDefinition.mName), static_cast<uint32_t>(drawn));
    if (drawn == D2DERR_RECREATE_TARGET) {
      mGpu.reset();// re-created on the next frame
    }
    return false;
  }

  ring.SetContentSize(slot, size);
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
  RenderStats stats;
  {
    std::scoped_lock lock(mStatsMutex);
    stats = mStats;
  }
  Log::Info("Host stats (60 s): {} frames, {} panel renders ({:.1f}/s), {} panels; viewers {}; layer process {}",
    mFramesSinceStats,
    mRendersSinceStats,
    static_cast<double>(mRendersSinceStats) / 60.0,
    mPanels.size(),
    stats.mHasViewers ? "yes" : "none",
    stats.mLayerProcessID);
  mFramesSinceStats = 0;
  mRendersSinceStats = 0;
  // One line per panel: with several panels, one line overran the log's
  // message limit and lost the later panels (round 9)
  for (const auto& panel: mPanels) {
    std::string text;
    if (panel->mSource) {
      text = panel->mSource->StatsText();
    }
    // A long name can't push the stats past the message limit
    const auto name = StringUtil::Narrow(panel->mDefinition.mName);
    Log::Info("Host stats, '{}': {} renders this minute, {} in all{}{}",
      StringUtil::TruncateUtf8(name, 60),
      panel->mRenders - panel->mRendersAtLastStats,
      panel->mRenders,
      text.empty() ? "" : "; ",
      text);
    panel->mRendersAtLastStats = panel->mRenders;
  }
}

void RenderLoop::Impl::Main() {
  if (!mHighResolutionTimer) {
    Log::Warning("Frame timer: standard resolution (15.6 ms steps); panels may update less often than their cap");
  }
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

  const HANDLE handles[] {mStop, mTimer, mLayerWake};
  for (;;) {
    // The 1 s timeout keeps the layer report, window re-finding and
    // shared-memory retries going
    const auto woke = WaitForMultipleObjects(3, handles, FALSE, 1000);
    if (woke == WAIT_OBJECT_0) {
      break;
    }
    const bool layerWoke = woke == WAIT_OBJECT_0 + 2;
    FrameWakeup::FrameStarted();
    const auto now = TimeUtil::Now();
    ++mFramesSinceStats;
    {
      std::scoped_lock lock(mStatsMutex);
      ++mStats.mFrames;
    }

    EnsurePublisher(now);
    if (const auto recenter = mRecenterCount.load(); mPublisher && mRecenterWritten != recenter) {
      mPublisher->WriteRecenterCount(recenter);
      mRecenterWritten = recenter;
    }
    ApplyPendingPanels();
    FollowHandsEnabled();
    if (layerWoke) {
      FollowFocus();// the head points at another panel
    }
    IssueDueSignals(now);
    if (mSecondTimer.Due(now)) {
      FollowLayer();
      FollowFocus();// in case a wake was missed (a layer that has gone)
      for (auto& panel: mPanels) {
        if (panel->mSource) {
          ThreadUtil::CatchAll("source maintenance", [&] { panel->mSource->Maintain(now); });
        }
      }
      UpdateUntrimmedSizes();
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
    bool publish = false;
    for (auto& panel: mPanels) {
      // Content that hides by itself (the keyboard): left out of what the
      // layer shows, and idle
      if (const bool sourceHidden = panel->mSource && panel->mSource->IsHiddenNow(); sourceHidden != panel->mSourceHidden) {
        panel->mSourceHidden = sourceHidden;
        publish = true;
      }
      const bool active = !panel->mHidden && !panel->mSourceHidden && viewers && panel->mSource;
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
        Publish();
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
      // A key press is drawn at once, past the cap (doc 07)
      const bool urgent = panel.mSource->TakeUrgent();
      const bool structural = (kind & Panels::Dirty::Structural) || urgent;
      // Is there really something new? (Identical captured frames are
      // dropped here; the GPU check answers a frame later.)
      if (kind & Panels::Dirty::Content) {
        const auto readiness = panel.mSource->Prepare();
        if (readiness == Render::Readiness::Pending) {
          panel.mDirty->Restore(Panels::Dirty::Content);
          FrameWakeup::ScheduleNextFrame(now + 2ms);
          if (!structural) {
            continue;
          }
        } else if (readiness == Render::Readiness::Unchanged && !structural) {
          continue;
        }
      }
      const auto floor = panel.mLastPublish + mOptions.mRingSafetyFloor;
      if (!panel.mLimiter.MayRender(now, structural) || (panel.mContentVersion > 0 && now < floor)) {
        panel.mDirty->Restore(urgent ? (kind | Panels::Dirty::Structural) : kind);// postponed, never dropped
        FrameWakeup::ScheduleNextFrame(std::max(panel.mLimiter.NextAllowed(), floor));
        continue;
      }
      bool rendered = false;
      ThreadUtil::CatchAll("panel render", [&] { rendered = RenderPanel(panel, now); });
      if (rendered) {
        publish = true;
        // Panels showing part of this one (the keyboard's preview) redraw,
        // within their own cap
        for (auto& other: mPanels) {
          if (other.get() != &panel && other->mSource && other->mSource->PartOfPanel() == panel.mDefinition.mID) {
            other->mDirty->Mark(Panels::Dirty::Content);
            FrameWakeup::RequestFrameSoon();
          }
        }
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
    // A panel that just finished hiding itself (the keyboard's fade) goes now
    for (auto& panel: mPanels) {
      if (const bool sourceHidden = panel->mSource && panel->mSource->IsHiddenNow(); sourceHidden != panel->mSourceHidden) {
        panel->mSourceHidden = sourceHidden;
        publish = true;
      }
    }
    if (publish) {
      Publish();
      UpdateUntrimmedSizes();// a window resized, or a new panel's first frame
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
