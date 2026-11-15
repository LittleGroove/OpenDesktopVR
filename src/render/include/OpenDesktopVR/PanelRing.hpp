// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// One panel's ring of shared textures and its fence (doc 02 "GPU-side sync",
// ADR-006):
// - 3 slots with shared NT handles, at maximum eviction priority (RN-13);
// - one shared fence, signalled with a new value per frame and flushed at
//   once (SY-3);
// - the host draws into the slot after the newest, so the layer can still
//   copy the newest (and the one before) while the next is drawn (SY-5).
#pragma once

#include <OpenDesktopVR/GpuDevice.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace OpenDesktopVR::Render {

class PanelRing {
 public:
  static constexpr uint32_t Slots = 3;

  // Null on failure (logged). `format` is B8G8R8A8_UNORM or R8G8B8A8_UNORM
  static std::unique_ptr<PanelRing> Create(GpuDevice& gpu, uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept;
  ~PanelRing();
  PanelRing(const PanelRing&) = delete;
  PanelRing& operator=(const PanelRing&) = delete;

  // The slot to draw the next frame into, and its Direct2D target
  uint32_t NextSlot() const noexcept {
    return (mNewestSlot + 1) % Slots;
  }
  ID2D1Bitmap1* Target(uint32_t slot) const noexcept {
    return mTargets[slot].get();
  }

  // Signals the fence for a frame drawn into `slot` and flushes, so the
  // signal can't sit in the command buffer (SY-3). Returns the fence value
  // that marks the frame complete.
  uint64_t Finish(ID3D11DeviceContext4* context, uint32_t slot) noexcept;
  // The value the next Finish will signal (to publish a frame before its
  // signal, which only the late-fence test does)
  uint64_t NextFenceValue() const noexcept {
    return mFenceValue + 1;
  }

  uint32_t NewestSlot() const noexcept {
    return mNewestSlot;
  }
  uint64_t FenceValue() const noexcept {
    return mFenceValue;
  }
  uint64_t TextureHandle(uint32_t slot) const noexcept {
    return reinterpret_cast<uint64_t>(mTextureHandles[slot]);
  }
  uint64_t FenceHandle() const noexcept {
    return reinterpret_cast<uint64_t>(mFenceHandle);
  }
  uint32_t Width() const noexcept {
    return mWidth;
  }
  uint32_t Height() const noexcept {
    return mHeight;
  }
  DXGI_FORMAT Format() const noexcept {
    return mFormat;
  }

 private:
  PanelRing() = default;

  std::array<winrt::com_ptr<ID3D11Texture2D>, Slots> mTextures;
  std::array<winrt::com_ptr<ID2D1Bitmap1>, Slots> mTargets;
  std::array<HANDLE, Slots> mTextureHandles {};
  winrt::com_ptr<ID3D11Fence> mFence;
  HANDLE mFenceHandle {nullptr};
  uint64_t mFenceValue {0};
  uint32_t mNewestSlot {Slots - 1};
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  DXGI_FORMAT mFormat {DXGI_FORMAT_UNKNOWN};
};

}// namespace OpenDesktopVR::Render
