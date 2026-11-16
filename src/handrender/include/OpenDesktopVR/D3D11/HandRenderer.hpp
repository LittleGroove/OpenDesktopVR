// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
#pragma once

#include <OpenDesktopVR/HandInteraction.hpp>

#include <winrt/base.h>

#include <d3d11.h>

#include <span>

namespace OpenDesktopVR::D3D11 {

// Draws see-through hands, styled after the Quest's hands
class HandRenderer final {
 public:
  using Quaternion = DirectX::SimpleMath::Quaternion;
  using Vector3 = DirectX::SimpleMath::Vector3;

  struct Eye {
    Vector3 mPosition {};
    // The eye looks along its local -Z, with +Y up
    Quaternion mOrientation {};
    // Tangents of the field of view's half-angles; left and down are negative
    float mTanLeft {};
    float mTanRight {};
    float mTanUp {};
    float mTanDown {};
  };

  HandRenderer() = delete;
  explicit HandRenderer(ID3D11Device*);
  ~HandRenderer();

  [[nodiscard]]
  static bool AnyVisible(std::span<const HandInteraction::RenderHand>) noexcept;

  // Draws into `viewport`, which the caller should already have cleared to
  // transparent black. Output is premultiplied alpha. Changes the context's
  // state; callers that need it preserved must save and restore it (e.g.
  // with ID3D11DeviceContext1::SwapDeviceContextState).
  void Render(
    ID3D11DeviceContext*,
    ID3D11RenderTargetView*,
    const D3D11_VIEWPORT&,
    const Eye&,
    std::span<const HandInteraction::RenderHand>);
  // The same, drawing only inside `scissor` (within the viewport): the
  // layer clears only around the hands, so nothing may land outside that
  // area (added in the port, M4a)
  void Render(
    ID3D11DeviceContext*,
    ID3D11RenderTargetView*,
    const D3D11_VIEWPORT&,
    const D3D11_RECT& scissor,
    const Eye&,
    std::span<const HandInteraction::RenderHand>);

 private:
  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11VertexShader> mConeVertexShader;
  winrt::com_ptr<ID3D11VertexShader> mSphereVertexShader;
  winrt::com_ptr<ID3D11PixelShader> mPixelShader;
  winrt::com_ptr<ID3D11Buffer> mConstantBuffer;
  winrt::com_ptr<ID3D11BlendState> mDepthOnlyBlendState;
  winrt::com_ptr<ID3D11BlendState> mColorBlendState;
  winrt::com_ptr<ID3D11DepthStencilState> mDepthWriteState;
  winrt::com_ptr<ID3D11DepthStencilState> mDepthEqualState;
  winrt::com_ptr<ID3D11RasterizerState> mRasterizerState;

  struct TextureSize {
    UINT mWidth {};
    UINT mHeight {};
    bool operator==(const TextureSize&) const = default;
  };
  TextureSize mDepthSize {};
  winrt::com_ptr<ID3D11Texture2D> mDepthTexture;
  winrt::com_ptr<ID3D11DepthStencilView> mDepthStencilView;

  void UpdateDepthBuffer(ID3D11RenderTargetView*);
};

}// namespace OpenDesktopVR::D3D11
