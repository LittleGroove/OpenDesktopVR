// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Building the layer list for the next xrEndFrame (doc 03, step 5):
// the game's layers unchanged and first, then our panels back to front, then
// the hands. Storage is sized once per session, so composing a frame never
// allocates (XR-3).
#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <span>
#include <vector>

namespace OpenDesktopVR::Layer {

// Room for our layers under the runtime's limit (XR-9):
// max(0, runtime maximum - game layers), never underflowing
constexpr uint32_t LayerRoom(uint32_t runtimeMaximum, uint32_t gameLayers) noexcept {
  return gameLayers >= runtimeMaximum ? 0 : runtimeMaximum - gameLayers;
}

// A panel ready to compose: its quad layer, and its distance from the head
// to the closest point of its rectangle (for back-to-front order)
struct ComposedPanel {
  const XrCompositionLayerQuad* mQuad {nullptr};
  float mDistance {0};
  bool mFocused {false};
};

struct CompositionResult {
  std::span<const XrCompositionLayerBaseHeader* const> mLayers;
  uint32_t mPanelsComposed {0};
  uint32_t mPanelsDropped {0};// no room: the farthest go first
  bool mHandLayerComposed {false};
};

class FrameComposer {
 public:
  // `capacity`: the most layers a frame can hold (the runtime's maximum is a
  // good choice; the game's own layers beyond it pass through unchanged)
  explicit FrameComposer(std::size_t capacity, std::size_t maxPanels);

  // `panels` is reordered in place (back to front). Never allocates.
  CompositionResult Compose(
    std::span<const XrCompositionLayerBaseHeader* const> gameLayers,
    std::span<ComposedPanel> panels,
    const XrCompositionLayerBaseHeader* handLayer,
    uint32_t runtimeMaximum) noexcept;

 private:
  std::vector<const XrCompositionLayerBaseHeader*> mLayers;
};

}// namespace OpenDesktopVR::Layer
