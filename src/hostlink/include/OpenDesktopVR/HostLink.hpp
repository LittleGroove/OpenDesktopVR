// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The client side of the shared block, for the layer and for tools (mirror
// viewer). Everything here is non-blocking and runs on the caller's frame
// thread (doc 02 "Host restarts and game restarts are normal events"):
// - with no host, it tries to open the block about once a second;
// - a version mismatch is logged once, and the client stays out (SY-7);
// - it checks the host process without waiting; when the host is gone it
//   drops everything, and re-attaches when a host returns;
// - a new session generation drops and re-opens every shared resource;
// - handles are pulled from the host process into this one (SY-9): the host
//   never opens us;
// - storage is fixed: no allocations per frame once panels are open.
#pragma once

#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <d3d11_4.h>

#include <winrt/base.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace OpenDesktopVR::HostLink {

struct PanelResources {
  uint64_t mPanelID {0};
  uint64_t mGeneration {0};
  uint32_t mTextureGeneration {0};
  std::array<winrt::com_ptr<ID3D11Texture2D>, SharedAbi::RingSlots> mTextures;
  winrt::com_ptr<ID3D11Fence> mFence;
  bool mFailed {false};// opening failed for this generation: not retried per frame
};

struct ClientOptions {
  std::chrono::milliseconds mReconnectInterval {1000};
  std::chrono::milliseconds mMismatchRetryInterval {5000};
  std::chrono::milliseconds mHostCheckInterval {250};
  // Claim the layer-status section (only layers do; tools just read)
  bool mWritesStatus {true};
};

struct ClientCounters {
  uint64_t mConnects {0};
  uint64_t mDisconnects {0};
  uint64_t mResourceOpens {0};
  uint64_t mOpenFailures {0};
};

class Client {
 public:
  // `device` is where shared textures and fences are opened (the game's
  // device in the layer). Fences need ID3D11Device5 (Windows 10 1703+).
  explicit Client(ID3D11Device* device, ClientOptions options = {});
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  bool IsUsable() const noexcept {
    return mDevice5 != nullptr;
  }

  // Once per frame. Cheap, never waits.
  void Update(TimeUtil::TimePoint now) noexcept;

  bool IsConnected() const noexcept {
    return mHostProcess != nullptr;
  }
  // Checked right now, not at the last Update: a GPU wait is only queued on
  // a fence whose host is still running, so it can't wait forever
  bool HostIsRunning() const noexcept;
  uint64_t Generation() const noexcept {
    return mGeneration;
  }
  // The latest consistent panel table (the previous one if a read clashed)
  std::span<const SharedAbi::PanelEntry> Panels() const noexcept {
    return {mSnapshot.mPanels, IsConnected() ? mSnapshot.mPanelCount : 0u};
  }

  // The shared textures and fence for a panel, opened on first use; null if
  // they can't be opened. Opening costs real time (test round 4: part of a
  // 24 ms frame), so it only happens while `openBudget` is true, and uses
  // it up: callers allow one per frame.
  const PanelResources* Resources(const SharedAbi::PanelEntry& entry, bool& openBudget) noexcept;

  // Layers: writes this process's report, claiming the status section if it
  // is free or its owner has exited. Only the owner writes.
  void WriteStatus(const SharedAbi::LayerStatus& status) noexcept;
  bool OwnsStatus() const noexcept {
    return mOwnsStatus;
  }
  // Tells the host to read the report now (it changed the focused panel).
  // Never waits.
  void WakeHost() noexcept;

  // Hands (doc 03 "Layer to host"). Only the status owner writes samples;
  // each one sets the host's tracking event. Never waits, never allocates.
  void WriteTrackingSample(SharedAbi::TrackingSample& sample) noexcept;
  // The host's latest hand state; all zero (hands off) while not connected
  // or if a read clashed twice in a row
  SharedAbi::HandState ReadHandState() noexcept;

  ClientCounters Counters() const noexcept {
    return mCounters;
  }

 private:
  void TryConnect() noexcept;
  void Disconnect(const char* why) noexcept;
  void DropResources() noexcept;
  bool Open(PanelResources& resources, const SharedAbi::PanelEntry& entry) noexcept;
  SharedAbi::Block* Block() const noexcept {
    return mMemory.As<SharedAbi::Block>();
  }

  ClientOptions mOptions;
  winrt::com_ptr<ID3D11Device1> mDevice1;
  winrt::com_ptr<ID3D11Device5> mDevice5;
  SharedMemory mMemory;
  HANDLE mHostProcess {nullptr};
  HANDLE mWakeEvent {nullptr};// pulled from the host
  HANDLE mTrackingEvent {nullptr};// pulled from the host
  SharedAbi::HandState mHandState {};
  uint32_t mHostProcessID {0};
  uint64_t mGeneration {0};
  SharedAbi::PanelTable mSnapshot {};
  std::array<PanelResources, SharedAbi::MaxPanels> mResources {};
  TimeUtil::TimePoint mNextAttempt {};
  TimeUtil::TimePoint mNextHostCheck {};
  TimeUtil::TimePoint mNextHeartbeat {};
  SharedMemory::Status mLastOpenStatus {SharedMemory::Status::NotFound};
  bool mLoggedMismatch {false};
  bool mOwnsStatus {false};
  TimeUtil::TimePoint mNextClaimCheck {};
  ClientCounters mCounters;
};

}// namespace OpenDesktopVR::HostLink
