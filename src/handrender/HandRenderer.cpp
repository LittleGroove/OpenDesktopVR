// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).

#include <OpenDesktopVR/D3D11/HandRenderer.hpp>
#include <OpenDesktopVR/Shaders/HandShaders.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace OpenDesktopVR::D3D11 {

using namespace HandInteraction;
using DirectX::SimpleMath::Matrix;

namespace {

// Must match Hand.hlsl
constexpr std::size_t MaxHands = 2;
constexpr std::size_t ConesPerHand = 24;
constexpr std::size_t SpheresPerHand = JointCount;
constexpr uint32_t ConeVertexCount = 12 * 6;// CONE_SEGMENTS quads
constexpr uint32_t SphereVertexCount = 8 * 12 * 6;// RINGS x SEGMENTS quads

struct Float4 {
  float x {}, y {}, z {}, w {};
};

struct Constants {
  DirectX::XMFLOAT4X4 mViewProjection;
  Float4 mEyePosition;
  std::array<Float4, MaxHands> mHandWrist;
  std::array<Float4, MaxHands> mHandAxis;
  std::array<Float4, MaxHands * ConesPerHand> mConeA;
  std::array<Float4, MaxHands * ConesPerHand> mConeB;
  std::array<Float4, MaxHands * SpheresPerHand> mSpheres;
};
static_assert(sizeof(Constants) % 16 == 0);

Float4 ToFloat4(const Vector3& v, float w) {
  return {v.x, v.y, v.z, w};
}

// The bones of each hand; each joint also gets a sphere
constexpr std::array<std::pair<Joint, Joint>, ConesPerHand> HandCones {{
  // Thumb
  {Joint::Wrist, Joint::ThumbMetacarpal},
  {Joint::ThumbMetacarpal, Joint::ThumbProximal},
  {Joint::ThumbProximal, Joint::ThumbDistal},
  {Joint::ThumbDistal, Joint::ThumbTip},
  // Palm
  {Joint::Wrist, Joint::IndexMetacarpal},
  {Joint::Wrist, Joint::LittleMetacarpal},
  {Joint::IndexMetacarpal, Joint::LittleMetacarpal},
  {Joint::IndexProximal, Joint::LittleProximal},
  // Index
  {Joint::IndexMetacarpal, Joint::IndexProximal},
  {Joint::IndexProximal, Joint::IndexIntermediate},
  {Joint::IndexIntermediate, Joint::IndexDistal},
  {Joint::IndexDistal, Joint::IndexTip},
  // Middle
  {Joint::MiddleMetacarpal, Joint::MiddleProximal},
  {Joint::MiddleProximal, Joint::MiddleIntermediate},
  {Joint::MiddleIntermediate, Joint::MiddleDistal},
  {Joint::MiddleDistal, Joint::MiddleTip},
  // Ring
  {Joint::RingMetacarpal, Joint::RingProximal},
  {Joint::RingProximal, Joint::RingIntermediate},
  {Joint::RingIntermediate, Joint::RingDistal},
  {Joint::RingDistal, Joint::RingTip},
  // Little
  {Joint::LittleMetacarpal, Joint::LittleProximal},
  {Joint::LittleProximal, Joint::LittleIntermediate},
  {Joint::LittleIntermediate, Joint::LittleDistal},
  {Joint::LittleDistal, Joint::LittleTip},
}};

}// namespace

HandRenderer::HandRenderer(ID3D11Device* device) {
  mDevice.copy_from(device);

  namespace Shaders = OpenDesktopVR::Shaders::Hand;
  winrt::check_hresult(device->CreateVertexShader(
    Shaders::ConeVS.data(),
    Shaders::ConeVS.size(),
    nullptr,
    mConeVertexShader.put()));
  winrt::check_hresult(device->CreateVertexShader(
    Shaders::SphereVS.data(),
    Shaders::SphereVS.size(),
    nullptr,
    mSphereVertexShader.put()));
  winrt::check_hresult(device->CreatePixelShader(
    Shaders::PS.data(), Shaders::PS.size(), nullptr, mPixelShader.put()));

  const D3D11_BUFFER_DESC constantBuffer {
    .ByteWidth = sizeof(Constants),
    .Usage = D3D11_USAGE_DEFAULT,
    .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
  };
  winrt::check_hresult(
    device->CreateBuffer(&constantBuffer, nullptr, mConstantBuffer.put()));

  // Pass 1 only writes depth
  D3D11_BLEND_DESC blend {};
  blend.RenderTarget[0].BlendEnable = FALSE;
  blend.RenderTarget[0].RenderTargetWriteMask = 0;
  winrt::check_hresult(
    device->CreateBlendState(&blend, mDepthOnlyBlendState.put()));
  // Pass 2 writes each pixel's nearest surface; no blending needed
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  winrt::check_hresult(
    device->CreateBlendState(&blend, mColorBlendState.put()));

  D3D11_DEPTH_STENCIL_DESC depth {
    .DepthEnable = TRUE,
    .DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL,
    .DepthFunc = D3D11_COMPARISON_LESS,
    .StencilEnable = FALSE,
  };
  winrt::check_hresult(
    device->CreateDepthStencilState(&depth, mDepthWriteState.put()));
  // Same shaders, so exactly the same depth values
  depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth.DepthFunc = D3D11_COMPARISON_EQUAL;
  winrt::check_hresult(
    device->CreateDepthStencilState(&depth, mDepthEqualState.put()));

  const D3D11_RASTERIZER_DESC rasterizer {
    .FillMode = D3D11_FILL_SOLID,
    .CullMode = D3D11_CULL_NONE,
    .DepthClipEnable = TRUE,
    .ScissorEnable = TRUE,
  };
  winrt::check_hresult(
    device->CreateRasterizerState(&rasterizer, mRasterizerState.put()));
}

HandRenderer::~HandRenderer() = default;

bool HandRenderer::AnyVisible(std::span<const RenderHand> hands) noexcept {
  return std::ranges::any_of(hands, [](const RenderHand& hand) {
    return hand.mVisibility > 0.001f && hand.mJoints.mIsTracked;
  });
}

void HandRenderer::UpdateDepthBuffer(ID3D11RenderTargetView* rtv) {
  winrt::com_ptr<ID3D11Resource> resource;
  rtv->GetResource(resource.put());
  D3D11_TEXTURE2D_DESC targetDesc {};
  resource.as<ID3D11Texture2D>()->GetDesc(&targetDesc);
  const TextureSize size {targetDesc.Width, targetDesc.Height};
  if (mDepthStencilView && size == mDepthSize) {
    return;
  }

  mDepthStencilView = nullptr;
  mDepthTexture = nullptr;
  const D3D11_TEXTURE2D_DESC depthDesc {
    .Width = size.mWidth,
    .Height = size.mHeight,
    .MipLevels = 1,
    .ArraySize = 1,
    .Format = DXGI_FORMAT_D32_FLOAT,
    .SampleDesc = {1, 0},
    .Usage = D3D11_USAGE_DEFAULT,
    .BindFlags = D3D11_BIND_DEPTH_STENCIL,
  };
  winrt::check_hresult(
    mDevice->CreateTexture2D(&depthDesc, nullptr, mDepthTexture.put()));
  winrt::check_hresult(mDevice->CreateDepthStencilView(
    mDepthTexture.get(), nullptr, mDepthStencilView.put()));
  mDepthSize = size;
}

void HandRenderer::Render(
  ID3D11DeviceContext* ctx,
  ID3D11RenderTargetView* rtv,
  const D3D11_VIEWPORT& viewport,
  const Eye& eye,
  std::span<const RenderHand> hands) {
  const D3D11_RECT scissor {
    .left = static_cast<LONG>(viewport.TopLeftX),
    .top = static_cast<LONG>(viewport.TopLeftY),
    .right = static_cast<LONG>(viewport.TopLeftX + viewport.Width),
    .bottom = static_cast<LONG>(viewport.TopLeftY + viewport.Height),
  };
  Render(ctx, rtv, viewport, scissor, eye, hands);
}

void HandRenderer::Render(
  ID3D11DeviceContext* ctx,
  ID3D11RenderTargetView* rtv,
  const D3D11_VIEWPORT& viewport,
  const D3D11_RECT& scissor,
  const Eye& eye,
  std::span<const RenderHand> hands) {
  Constants constants {};

  // OpenXR is right-handed, with the eye looking along -Z
  constexpr float nearZ = 0.02f;
  constexpr float farZ = 20.0f;
  const Matrix eyeToWorld = Matrix::CreateFromQuaternion(eye.mOrientation)
    * Matrix::CreateTranslation(eye.mPosition);
  const Matrix projection = DirectX::XMMatrixPerspectiveOffCenterRH(
    eye.mTanLeft * nearZ,
    eye.mTanRight * nearZ,
    eye.mTanDown * nearZ,
    eye.mTanUp * nearZ,
    nearZ,
    farZ);
  constants.mViewProjection = eyeToWorld.Invert() * projection;
  constants.mEyePosition = ToFloat4(eye.mPosition, 1);

  uint32_t handCount = 0;
  for (const auto& hand: hands) {
    if (hand.mVisibility <= 0.001f || !hand.mJoints.mIsTracked) {
      continue;
    }
    if (handCount >= MaxHands) {
      break;
    }
    const auto& joints = hand.mJoints;

    const Vector3 wrist = joints[Joint::Wrist].mPosition;
    Vector3 axis = joints[Joint::MiddleProximal].mPosition - wrist;
    axis.Normalize();
    constants.mHandWrist[handCount] = ToFloat4(wrist, hand.mVisibility);
    constants.mHandAxis[handCount] = ToFloat4(axis, 0);

    for (std::size_t i = 0; i < ConesPerHand; ++i) {
      const auto& [a, b] = HandCones[i];
      const auto index = (handCount * ConesPerHand) + i;
      constants.mConeA[index]
        = ToFloat4(joints[a].mPosition, joints[a].mRadius);
      constants.mConeB[index]
        = ToFloat4(joints[b].mPosition, joints[b].mRadius);
    }
    for (std::size_t i = 0; i < SpheresPerHand; ++i) {
      // The palm joint is in the middle of the hand; a sphere there bulges
      // out of the back of the hand, so give it no size
      const float radius = (static_cast<Joint>(i) == Joint::Palm)
        ? 0.0f
        : joints.mJoints[i].mRadius;
      constants.mSpheres[(handCount * SpheresPerHand) + i]
        = ToFloat4(joints.mJoints[i].mPosition, radius);
    }
    ++handCount;
  }
  if (handCount == 0) {
    return;
  }

  UpdateDepthBuffer(rtv);
  ctx->UpdateSubresource(mConstantBuffer.get(), 0, nullptr, &constants, 0, 0);
  // Other eyes use other parts of the target, and are already drawn
  ctx->ClearDepthStencilView(
    mDepthStencilView.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

  ctx->OMSetRenderTargets(1, &rtv, mDepthStencilView.get());
  ctx->RSSetState(mRasterizerState.get());
  ctx->RSSetViewports(1, &viewport);
  ctx->RSSetScissorRects(1, &scissor);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->HSSetShader(nullptr, nullptr, 0);
  ctx->DSSetShader(nullptr, nullptr, 0);
  ID3D11Buffer* constantBuffers[] = {mConstantBuffer.get()};
  ctx->VSSetConstantBuffers(0, 1, constantBuffers);
  ctx->PSSetConstantBuffers(0, 1, constantBuffers);

  const auto drawGeometry = [&]() {
    ctx->VSSetShader(mConeVertexShader.get(), nullptr, 0);
    ctx->DrawInstanced(ConeVertexCount, handCount * ConesPerHand, 0, 0);
    ctx->VSSetShader(mSphereVertexShader.get(), nullptr, 0);
    ctx->DrawInstanced(SphereVertexCount, handCount * SpheresPerHand, 0, 0);
  };

  // Pass 1: depth only, to find the nearest surface
  ctx->OMSetBlendState(mDepthOnlyBlendState.get(), nullptr, 0xffffffff);
  ctx->OMSetDepthStencilState(mDepthWriteState.get(), 0);
  ctx->PSSetShader(nullptr, nullptr, 0);
  drawGeometry();

  // Pass 2: color, only for the nearest surface
  ctx->OMSetBlendState(mColorBlendState.get(), nullptr, 0xffffffff);
  ctx->OMSetDepthStencilState(mDepthEqualState.get(), 0);
  ctx->PSSetShader(mPixelShader.get(), nullptr, 0);
  drawGeometry();
}

}// namespace OpenDesktopVR::D3D11
