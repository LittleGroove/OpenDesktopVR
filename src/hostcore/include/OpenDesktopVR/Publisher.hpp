// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The host's side of the shared block: creates it (only the host does,
// SY-8), writes the header and the panel table, and reads what the layer
// reports. The render thread is the only writer of the panel table.
#pragma once

#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace OpenDesktopVR::Host {

class Publisher {
 public:
  // Null if the block can't be created; `status` says why (an elevated game
  // holding the name is "access denied")
  static std::optional<Publisher> Create(SharedMemory::Status& status) noexcept;

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

 private:
  explicit Publisher(SharedMemory memory) noexcept : mMemory(std::move(memory)) {
  }
  SharedAbi::Block& Block() const noexcept {
    return *mMemory.As<SharedAbi::Block>();
  }

  SharedMemory mMemory;
};

}// namespace OpenDesktopVR::Host
