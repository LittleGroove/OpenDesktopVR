// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// One panel's OpenXR swapchain on D3D11 (ADR-016: one swapchain per panel).
// - Created once, at the host's texture size; re-created only when that
//   grows (RN-7) or after a failed image wait (XR-15). Each image holds a
//   frame of any size up to it, top-left; the quad shows just that part.
// - Tracks the "what's shown" key (SY-4): reset on (re)creation and before
//   each draw, set only after a successful release. Unchanged content is
//   never copied again; the runtime keeps showing the last released image.
#pragma once

#include "Dispatch.hpp"

#include <winrt/base.h>

#include <cstdint>
#include <vector>

namespace OpenDesktopVR::Layer {

class PanelSwapchain {
 public:
  PanelSwapchain(const NextFunctions& next, XrSession session, DXGI_FORMAT format) noexcept;
  ~PanelSwapchain();
  PanelSwapchain(const PanelSwapchain&) = delete;
  PanelSwapchain& operator=(const PanelSwapchain&) = delete;

  // Creates the swapchain, or re-creates it after a size change or a failed
  // wait. Allocates only when it creates. False if creation failed.
  bool Ensure(uint32_t width, uint32_t height) noexcept;
  // True if Ensure would create (or re-create) the swapchain
  bool NeedsCreate(uint32_t width, uint32_t height) const noexcept {
    return mSwapchain == XR_NULL_HANDLE || mNeedsRecreate || width != mWidth || height != mHeight;
  }

  // Copies the top-left `contentWidth` x `contentHeight` of `source` (same
  // format family; one more pixel each way where there's room, the host's
  // transparent edge) into a new image and releases it, tagging the image
  // with `contentKey`. Call inside the private context state. False on
  // failure; a failed wait schedules a re-create.
  bool Present(ID3D11DeviceContext* context, ID3D11Texture2D* source, uint64_t contentKey, uint32_t contentWidth,
    uint32_t contentHeight) noexcept;

  XrSwapchain Handle() const noexcept {
    return mSwapchain;
  }
  // 0 = nothing shown (new swapchain, or the last draw failed)
  uint64_t ShownKey() const noexcept {
    return mShownKey;
  }
  bool HasReleasedImage() const noexcept {
    return mShownKey != 0;
  }
  uint32_t Width() const noexcept {
    return mWidth;
  }
  uint32_t Height() const noexcept {
    return mHeight;
  }
  uint64_t ImagesReleased() const noexcept {
    return mImagesReleased;
  }
  // The size of the frame in the last released image (0 x 0 if none)
  uint32_t ShownWidth() const noexcept {
    return mShownWidth;
  }
  uint32_t ShownHeight() const noexcept {
    return mShownHeight;
  }

 private:
  void Destroy() noexcept;

  const NextFunctions& mNext;
  XrSession mSession {XR_NULL_HANDLE};
  DXGI_FORMAT mFormat {DXGI_FORMAT_UNKNOWN};
  XrSwapchain mSwapchain {XR_NULL_HANDLE};
  std::vector<winrt::com_ptr<ID3D11Texture2D>> mImages;
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  bool mNeedsRecreate {false};
  uint64_t mShownKey {0};
  uint32_t mShownWidth {0};
  uint32_t mShownHeight {0};
  uint64_t mImagesReleased {0};
};

}// namespace OpenDesktopVR::Layer
