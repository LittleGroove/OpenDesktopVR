// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Layer/Composition.hpp>

#include <algorithm>

namespace OpenDesktopVR::Layer {

FrameComposer::FrameComposer(std::size_t capacity, std::size_t maxPanels) {
  mLayers.reserve(capacity + maxPanels + 1);
}

CompositionResult FrameComposer::Compose(
  std::span<const XrCompositionLayerBaseHeader* const> gameLayers,
  std::span<ComposedPanel> panels,
  const XrCompositionLayerBaseHeader* handLayer,
  uint32_t runtimeMaximum) noexcept {
  CompositionResult result;
  mLayers.clear();
  if (gameLayers.size() + panels.size() + 1 > mLayers.capacity()) {
    // More than the session was sized for: show the game's frame unchanged
    result.mLayers = gameLayers;
    result.mPanelsDropped = static_cast<uint32_t>(panels.size());
    return result;
  }
  mLayers.insert(mLayers.end(), gameLayers.begin(), gameLayers.end());

  uint32_t room = LayerRoom(runtimeMaximum, static_cast<uint32_t>(gameLayers.size()));
  // The hand layer goes last but is placed first: hands matter more than
  // the farthest panel
  const bool hands = handLayer && room > 0;
  if (hands) {
    --room;
  }

  // Back to front: farthest first; on a tie, the focused panel goes on top
  // (std::sort with this order is allocation-free)
  std::sort(panels.begin(), panels.end(), [](const ComposedPanel& a, const ComposedPanel& b) {
    if (a.mDistance != b.mDistance) {
      return a.mDistance > b.mDistance;
    }
    return !a.mFocused && b.mFocused;
  });
  const auto keep = std::min<std::size_t>(room, panels.size());
  const auto dropped = panels.size() - keep;// the farthest ones
  for (std::size_t i = dropped; i < panels.size(); ++i) {
    mLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(panels[i].mQuad));
  }
  if (hands) {
    mLayers.push_back(handLayer);
  }

  result.mLayers = mLayers;
  result.mPanelsComposed = static_cast<uint32_t>(keep);
  result.mPanelsDropped = static_cast<uint32_t>(dropped);
  result.mHandLayerComposed = hands;
  return result;
}

}// namespace OpenDesktopVR::Layer
