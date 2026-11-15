// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/D3D11Util.hpp>

namespace OpenDesktopVR::D3D11Util {

PrivateContextState::PrivateContextState(ID3D11Device* device) noexcept {
  if (!device) {
    return;
  }
  winrt::com_ptr<ID3D11Device1> device1;
  if (FAILED(device->QueryInterface(device1.put()))) {
    return;
  }
  UINT flags = 0;
  if (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) {
    flags |= D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED;
  }
  const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
  device1->CreateDeviceContextState(
    flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, mState.put());
}

PrivateContextState::Scope::Scope(ID3D11DeviceContext1* context, ID3DDeviceContextState* state) noexcept {
  if (context && state) {
    mContext = context;
    mContext->SwapDeviceContextState(state, mPrevious.put());
  }
}

PrivateContextState::Scope::~Scope() {
  if (mContext && mPrevious) {
    mContext->SwapDeviceContextState(mPrevious.get(), nullptr);
  }
}

std::optional<FormatInfo> DescribeColorFormat(DXGI_FORMAT format) noexcept {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return FormatInfo {format, DXGI_FORMAT_B8G8R8A8_TYPELESS, ChannelOrder::BGRA, true};
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return FormatInfo {format, DXGI_FORMAT_B8G8R8A8_TYPELESS, ChannelOrder::BGRA, false};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return FormatInfo {format, DXGI_FORMAT_R8G8B8A8_TYPELESS, ChannelOrder::RGBA, true};
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return FormatInfo {format, DXGI_FORMAT_R8G8B8A8_TYPELESS, ChannelOrder::RGBA, false};
    default:
      return std::nullopt;
  }
}

std::optional<DXGI_FORMAT> ChoosePanelFormat(std::span<const int64_t> offered) noexcept {
  std::optional<DXGI_FORMAT> firstLinear;
  for (const auto value: offered) {
    const auto format = static_cast<DXGI_FORMAT>(value);
    const auto info = DescribeColorFormat(format);
    if (!info) {
      continue;
    }
    if (info->mSRGB) {
      return format;
    }
    if (!firstLinear) {
      firstLinear = format;
    }
  }
  return firstLinear;
}

winrt::com_ptr<ID3D11Texture2D> CreateStaticTexture(
  ID3D11Device* device,
  DXGI_FORMAT format,
  uint32_t width,
  uint32_t height,
  const void* pixels) noexcept {
  winrt::com_ptr<ID3D11Texture2D> texture;
  if (!device || width == 0 || height == 0) {
    return texture;
  }
  const D3D11_TEXTURE2D_DESC desc {
    .Width = width,
    .Height = height,
    .MipLevels = 1,
    .ArraySize = 1,
    .Format = format,
    .SampleDesc = {1, 0},
    .Usage = D3D11_USAGE_IMMUTABLE,
    .BindFlags = D3D11_BIND_SHADER_RESOURCE,
  };
  const D3D11_SUBRESOURCE_DATA data {.pSysMem = pixels, .SysMemPitch = width * 4};
  if (FAILED(device->CreateTexture2D(&desc, pixels ? &data : nullptr, texture.put()))) {
    texture = nullptr;
  }
  return texture;
}

}// namespace OpenDesktopVR::D3D11Util
