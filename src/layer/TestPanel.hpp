// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The built-in test panel (M1): a static test card, world-locked in front of
// where the head was on the first tracked frame. Its image is copied once;
// after that it costs one quad layer and no GPU work.
//
// Per frame: Prepare(head) -> if true, Draw(context) inside the private
// context state -> Quad().
#pragma once

#include "Dispatch.hpp"
#include "PanelSwapchain.hpp"

#include <OpenDesktopVR/Layer/TestPattern.hpp>

#include <winrt/base.h>

#include <optional>

namespace OpenDesktopVR::Layer {

struct TestPanelOptions {
  float mWidthMeters {0.40f};
  float mDistance {0.55f};// in front of the head, level
  float mDrop {0.20f};    // below eye height
  TestPatternOptions mPattern {};
};

class TestPanel {
 public:
  TestPanel(
    const NextFunctions& next,
    XrSession session,
    XrSpace localSpace,
    ID3D11Device* device,
    DXGI_FORMAT format,
    const TestPanelOptions& options = {});

  bool NeedsHeadPose() const noexcept {
    return !mPlaced && !mFailed;
  }
  // Places the panel (once, from `head`) and makes sure it has a swapchain.
  // True if its image must be drawn this frame.
  bool Prepare(const std::optional<XrPosef>& head) noexcept;
  // Copies the test card into a new image; true on success
  bool Draw(ID3D11DeviceContext* context) noexcept;
  // The quad layer to submit, or null if there's nothing to show yet
  const XrCompositionLayerQuad* Quad() noexcept;

  bool HasFailed() const noexcept {
    return mFailed;
  }

 private:
  static constexpr uint64_t ContentKey = 1;// the content never changes

  TestPanelOptions mOptions;
  PanelSwapchain mSwapchain;
  winrt::com_ptr<ID3D11Texture2D> mTexture;
  XrCompositionLayerQuad mQuad {XR_TYPE_COMPOSITION_LAYER_QUAD};
  bool mPlaced {false};
  bool mFailed {false};
};

}// namespace OpenDesktopVR::Layer
