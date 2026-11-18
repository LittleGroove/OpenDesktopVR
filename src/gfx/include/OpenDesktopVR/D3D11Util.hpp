// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Direct3D 11 helpers shared by the layer and the host: isolating pipeline
// state on a shared immediate context (XR-20), format families, and texture
// creation.
#pragma once

#include <d3d11_1.h>
#include <dxgiformat.h>

#include <winrt/base.h>

#include <cstdint>
#include <optional>
#include <span>

namespace OpenDesktopVR::D3D11Util {

// A context state of our own, created once per device. Swapping it in hides
// every pipeline change from the game, and swapping back restores the game's
// state exactly (doc 03 "D3D11 state hygiene").
class PrivateContextState {
 public:
  // Null state on failure (old runtime without D3D11.1): Scope then does
  // nothing, and the caller should only use calls that don't touch pipeline
  // state (copies)
  explicit PrivateContextState(ID3D11Device* device) noexcept;
  bool IsValid() const noexcept {
    return mState != nullptr;
  }

  class [[nodiscard]] Scope {
   public:
    Scope(ID3D11DeviceContext1* context, ID3DDeviceContextState* state) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    ID3D11DeviceContext1* mContext {nullptr};
    winrt::com_ptr<ID3DDeviceContextState> mPrevious;
  };

  Scope Enter(ID3D11DeviceContext1* context) const noexcept {
    return Scope {context, mState.get()};
  }

 private:
  winrt::com_ptr<ID3DDeviceContextState> mState;
};

enum class ChannelOrder { BGRA, RGBA };

struct FormatInfo {
  DXGI_FORMAT mTyped {DXGI_FORMAT_UNKNOWN};
  DXGI_FORMAT mTypeless {DXGI_FORMAT_UNKNOWN};
  ChannelOrder mOrder {ChannelOrder::BGRA};
  bool mSRGB {false};
};

// For the 8-bit BGRA/RGBA formats we render panels in; nullopt otherwise
std::optional<FormatInfo> DescribeColorFormat(DXGI_FORMAT format) noexcept;

// The first of `offered` (in the runtime's preference order) that we can
// fill, preferring sRGB variants when the runtime offers both
std::optional<DXGI_FORMAT> ChoosePanelFormat(std::span<const int64_t> offered) noexcept;

// An immutable texture with `pixels` (4 bytes each, tightly packed)
winrt::com_ptr<ID3D11Texture2D> CreateStaticTexture(
  ID3D11Device* device,
  DXGI_FORMAT format,
  uint32_t width,
  uint32_t height,
  const void* pixels) noexcept;

}// namespace OpenDesktopVR::D3D11Util
