// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The host's side of the shared block: creates it (only the host does,
// SY-8), writes the header and the panel table, and reads what the layer
// reports. The render thread is the only writer of the panel table; the
// hand thread is the only reader of the tracking ring and the only writer
// of the hand state (both safe from any one thread at a time).
#pragma once

#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace OpenDesktopVR::Host {

class Publisher {
 public:
  // Null if the block can't be created; `status` says why (an elevated game
  // holding the name is "access denied"). `wakeEvent` (owned by the
  // caller, outliving the block) is set by a layer whose report needs
  // acting on at once; `trackingEvent` (likewise) after each tracking sample.
  static std::optional<Publisher> Create(
    SharedMemory::Status& status, void* wakeEvent = nullptr, void* trackingEvent = nullptr) noexcept;

  Publisher(Publisher&&) noexcept = default;
  Publisher& operator=(Publisher&&) noexcept = default;

  // Layers drop and re-open every shared resource when this changes
  uint64_t Generation() const noexcept;
  void BumpGeneration() noexcept;

  void PublishPanels(std::span<const SharedAbi::PanelEntry> panels) noexcept;

  // True if a layer or the mirror looked within `window`
  bool HasViewers(uint64_t windowMilliseconds = 3000) const noexcept;

  // The layer's latest report; nullopt if none is consistent right now
  std::optional<SharedAbi::LayerStatus> ReadStatus() const noexcept;

  // Samples written so far (the newest sample's index)
  uint64_t TrackingWritten() const noexcept;
  // Copies sample `index` if it's still in the ring and consistent
  bool ReadTrackingSample(uint64_t index, SharedAbi::TrackingSample& out) const noexcept;
  // What the layer draws of each hand, and whether it should send samples
  void WriteHandState(const SharedAbi::HandState& state) noexcept;
  // How many times the "Recenter" keybind was used (ADR-037)
  void WriteRecenterCount(uint64_t count) noexcept;

 private:
  explicit Publisher(SharedMemory memory) noexcept : mMemory(std::move(memory)) {
  }
  SharedAbi::Block& Block() const noexcept {
    return *mMemory.As<SharedAbi::Block>();
  }

  SharedMemory mMemory;
};

}// namespace OpenDesktopVR::Host
