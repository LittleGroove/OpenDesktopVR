// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// One panel's ring of shared textures and its fence (doc 02 "GPU-side sync",
// ADR-006):
// - 3 slots with shared NT handles, at maximum eviction priority (RN-13);
// - one shared fence, signalled with a new value per frame and flushed at
//   once (SY-3);
// - the host draws into the slot after the newest, so the layer can still
//   copy the newest (and the one before) while the next is drawn (SY-5);
// - textures only grow (RN-7): a smaller frame uses the top-left part, and
//   each slot remembers its frame's size, so resizing or trimming a window
//   re-creates nothing here or in the game.
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
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

  // The texture size for content of `size`, given the current one (0 x 0 if
  // none): never smaller than the current size, and rounded up to a step,
  // so a window dragged larger re-creates the ring every 256 pixels, not
  // every pixel
  static PixelSize CapacityFor(PixelSize content, PixelSize current) noexcept;
  bool Fits(PixelSize content) const noexcept {
    return content.mWidth <= mWidth && content.mHeight <= mHeight;
  }
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
  // The same texture to draw from (another panel showing part of this one:
  // the keyboard's preview strip, K13); null if Direct2D couldn't make it
  ID2D1Bitmap1* Source(uint32_t slot) const noexcept {
    return mSources[slot % Slots].get();
  }

  // Signals the fence for a frame drawn into `slot` and flushes, so the
  // signal can't sit in the command buffer (SY-3). Returns the fence value
  // that marks the frame complete.
  uint64_t Finish(ID3D11DeviceContext4* context, uint32_t slot) noexcept {
    const auto value = Advance(slot);
    Signal(context, value);
    return value;
  }
  // The two halves of Finish, for the late-fence test only: a frame is
  // announced (Advance) and its fence signalled later (Signal)
  uint64_t Advance(uint32_t slot) noexcept;

  // The size of the frame drawn into `slot` (top-left); 0 x 0 until drawn
  void SetContentSize(uint32_t slot, PixelSize size) noexcept {
    mContentSizes[slot % Slots] = size;
  }
  PixelSize ContentSize(uint32_t slot) const noexcept {
    return mContentSizes[slot % Slots];
  }
  void Signal(ID3D11DeviceContext4* context, uint64_t value) noexcept;

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
  std::array<winrt::com_ptr<ID2D1Bitmap1>, Slots> mSources;
  std::array<HANDLE, Slots> mTextureHandles {};
  winrt::com_ptr<ID3D11Fence> mFence;
  HANDLE mFenceHandle {nullptr};
  uint64_t mFenceValue {0};
  uint32_t mNewestSlot {Slots - 1};
  std::array<PixelSize, Slots> mContentSizes {};
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  DXGI_FORMAT mFormat {DXGI_FORMAT_UNKNOWN};
};

}// namespace OpenDesktopVR::Render
