// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The host's render thread (doc 02 "Threads", doc 08 "Render loop"):
// - wakes only for dirty panels (FrameWakeup timer, at least 8 ms apart);
// - takes each dirty flag before drawing (RN-2) and respects each panel's
//   cap (RN-5); structural changes bypass the cap but never the ring's
//   safety floor (SY-5);
// - draws into the next ring slot, signals, flushes (SY-3), then publishes;
// - draws nothing while no viewer (a layer, the mirror) is watching, and
//   tells sources to stop producing frames (CP-5);
// - about once a second: follows the game's adapter and panel format,
//   maintains sources (re-finding windows), retries shared memory (SY-8);
// - ring textures only grow (RN-7); each frame says how much of them it uses;
// - panels can be replaced at any time from any thread (SetPanels); a panel
//   whose content is unchanged keeps its source; a hidden one (PN-6) keeps
//   it too, idle (CP-5), and isn't shown;
// - each panel's look (opacity, brightness, tint: PN-4, PN-5) is applied
//   as it's drawn, with its "looked at" opacity while the layer reports the
//   head pointing at it (the layer sets a wake event: no polling);
// - writes a stats line every 60 s (LG-2), one line per panel;
// - carries hands between the layer and the hand thread: the tracking
//   event and ring, the hand state, and the panels the hands may touch as
//   last published (doc 03 "Layer to host", "Host to layer");
// - passes "Recenter" on to the layer (ADR-037): a count in the shared
//   block, which the layer turns into a recenter at the head.
#pragma once

#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>

#include <atomic>
#include <chrono>
#include <set>
#include <functional>
#include <memory>
#include <optional>
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
  // Test only: signal each frame's fence this long after publishing it,
  // while rendering continues (a host whose GPU work is starved), to
  // exercise the layer's bounded staleness (ADR-012, ADR-024)
  std::chrono::milliseconds mFenceDelay {0};
  // Test only: draw even with nobody watching
  bool mRenderWithoutViewers {false};
  bool mAllowWarp {false};
};

// Makes the content for a panel, on the render thread, once the GPU exists
using SourceFactory = std::function<std::unique_ptr<Render::ContentSource>(
  const PanelDefinition& definition,
  Render::GpuDevice& gpu,
  Render::MarkDirty markDirty)>;

struct RenderStats {
  uint64_t mFrames {0};
  uint64_t mRenders {0};
  uint64_t mPublishes {0};
  uint64_t mDeviceCreations {0};
  bool mBlockCreated {false};
  bool mHasViewers {false};
  uint32_t mLayerProcessID {0};
  // The game it's in, as the layer reports it (per-game profiles, ADR-038)
  std::wstring mLayerExecutable;
  // The layer's last report, for the diagnostics page and the self-test
  // (M6c); nullopt until a layer attaches. Kept after the game ends.
  std::optional<SharedAbi::LayerStatus> mLayerStatus;
  // When that layer attached
  std::chrono::steady_clock::time_point mLayerAttachedAt {};
};

class RenderLoop {
 public:
  explicit RenderLoop(RenderLoopOptions options, SourceFactory factory);
  ~RenderLoop();
  RenderLoop(const RenderLoop&) = delete;
  RenderLoop& operator=(const RenderLoop&) = delete;

  // The panels (enabled ones only), and which of them are hidden for now;
  // safe from any thread, applied on the render thread's next frame
  void SetPanels(std::vector<PanelDefinition> panels, std::set<uint64_t> hidden = {});

  void Start();
  void Stop() noexcept;

  RenderStats Stats() const noexcept;

  // A panel's content size before trimming (a window's client area), for
  // the settings window's trim ranges; nullopt while unknown. Any thread.
  std::optional<Render::PixelSize> UntrimmedSize(uint64_t panelID) const noexcept;

  // The hand thread's link to the layer: samples in, hand state out. The
  // last hand state written is kept and written again when the shared
  // block is (re)created. Valid for the render loop's lifetime.
  TrackingChannel& Tracking() noexcept;
  // The panels the layer shows, as last published (any thread)
  std::shared_ptr<const TouchPanels> TouchablePanels() const;
  // "Use my hands" (any thread): with it off, no app is readied for input
  void SetHandsEnabled(bool enabled);
  // The "Recenter" keybind (any thread): panels go where the head is now
  void Recenter();
  // PN-12 (any thread): every panel's cap is clamped to this
  void SetGlobalMaxUpdatesPerSecond(double perSecond);

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

}// namespace OpenDesktopVR::Host
