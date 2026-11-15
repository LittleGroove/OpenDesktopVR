// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The host's render thread (doc 02 "Threads", doc 08 "Render loop"):
// - wakes only for dirty panels (FrameWakeup timer, at least 8 ms apart);
// - takes each dirty flag before drawing (RN-2) and respects each panel's
//   cap (RN-5); structural changes bypass the cap but never the ring's
//   safety floor (SY-5);
// - draws into the next ring slot, signals, flushes (SY-3), then publishes;
// - about once a second, reads what the layer reports and follows the game:
//   its adapter (device re-created, generation bumped) and its panel format
//   (rings re-created);
// - creates the shared block, and retries every few seconds if it can't
//   (SY-8);
// - writes a stats line every 60 s (LG-2).
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace OpenDesktopVR::Render {
class GpuDevice;
}

namespace OpenDesktopVR::Host {

struct RenderLoopOptions {
  double mGlobalMaxUpdatesPerSecond {Panels::DefaultMaxUpdatesPerSecond};
  // Ring safety (SY-5): a slot may be copied up to ~40 ms + a frame after
  // it's published, and is reused 3 publishes later, so publishes of one
  // panel are never closer than this, even for structural changes
  std::chrono::milliseconds mRingSafetyFloor {20};
  // Test only: publish each frame this long before signalling its fence, to
  // exercise the layer's bounded staleness (ADR-012)
  std::chrono::milliseconds mFenceDelay {0};
  bool mAllowWarp {false};
};

using SourceFactory = std::function<std::unique_ptr<Render::ContentSource>(Render::GpuDevice&)>;

struct RenderStats {
  uint64_t mFrames {0};
  uint64_t mRenders {0};
  uint64_t mPublishes {0};
  uint64_t mDeviceCreations {0};
  bool mBlockCreated {false};
  uint32_t mLayerProcessID {0};
};

class RenderLoop {
 public:
  explicit RenderLoop(RenderLoopOptions options = {});
  ~RenderLoop();
  RenderLoop(const RenderLoop&) = delete;
  RenderLoop& operator=(const RenderLoop&) = delete;

  // Before Start(): sources are made on the render thread, once the GPU exists
  void AddPanel(PanelDefinition definition, SourceFactory makeSource);

  void Start();
  void Stop() noexcept;

  // Global show/hide (doc 09 PN-6); safe from any thread
  void SetPanelsVisible(bool visible) noexcept;
  bool PanelsVisible() const noexcept {
    return mPanelsVisible.load();
  }

  RenderStats Stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
  std::atomic<bool> mPanelsVisible {true};
};

}// namespace OpenDesktopVR::Host
