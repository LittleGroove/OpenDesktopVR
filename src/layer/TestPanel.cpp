// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "TestPanel.hpp"

#include <OpenDesktopVR/D3D11Util.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

namespace OpenDesktopVR::Layer {

TestPanel::TestPanel(
  const NextFunctions& next,
  XrSession session,
  XrSpace localSpace,
  ID3D11Device* device,
  DXGI_FORMAT format,
  const TestPanelOptions& options)
  : mOptions(options), mSwapchain(next, session, format) {
  const auto info = D3D11Util::DescribeColorFormat(format);
  if (!info) {
    mFailed = true;
    return;
  }
  auto pattern = mOptions.mPattern;
  pattern.mOrder = info->mOrder == D3D11Util::ChannelOrder::BGRA ? PixelOrder::BGRA : PixelOrder::RGBA;
  const auto pixels = MakeTestPattern(pattern);
  mTexture = D3D11Util::CreateStaticTexture(device, format, pattern.mWidth, pattern.mHeight, pixels.data());
  if (!mTexture) {
    Log::Error("Test panel: couldn't create its texture");
    mFailed = true;
    return;
  }
  const float aspect = static_cast<float>(pattern.mHeight) / static_cast<float>(pattern.mWidth);
  // Premultiplied alpha, OpenXR's default; never the unpremultiplied bit
  mQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mQuad.space = localSpace;
  mQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  mQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(pattern.mWidth), static_cast<int32_t>(pattern.mHeight)}};
  mQuad.subImage.imageArrayIndex = 0;
  mQuad.size = {mOptions.mWidthMeters, mOptions.mWidthMeters * aspect};
}

bool TestPanel::Prepare(const std::optional<XrPosef>& head) noexcept {
  if (mFailed) {
    return false;
  }
  if (!mPlaced) {
    if (!head) {
      return false;// wait for a tracked head pose
    }
    mQuad.pose = XrUtil::PlaceInFront(*head, mOptions.mDistance, mOptions.mDrop);
    mPlaced = true;
    Log::Info(
      "Test panel placed at ({:.2f}, {:.2f}, {:.2f}), {:.2f} m x {:.2f} m",
      mQuad.pose.position.x,
      mQuad.pose.position.y,
      mQuad.pose.position.z,
      mQuad.size.width,
      mQuad.size.height);
  }
  const auto& pattern = mOptions.mPattern;
  if (!mSwapchain.Ensure(pattern.mWidth, pattern.mHeight)) {
    // Not retried every frame (XR-2): the test panel is off for this session
    Log::Error("Test panel: no swapchain; the test panel is off for this session");
    mFailed = true;
    return false;
  }
  return mSwapchain.ShownKey() != ContentKey;
}

bool TestPanel::Draw(ID3D11DeviceContext* context) noexcept {
  return !mFailed && mSwapchain.Present(context, mTexture.get(), ContentKey);
}

const XrCompositionLayerQuad* TestPanel::Quad() noexcept {
  if (mFailed || !mPlaced || !mSwapchain.HasReleasedImage()) {
    return nullptr;
  }
  mQuad.subImage.swapchain = mSwapchain.Handle();
  return &mQuad;
}

}// namespace OpenDesktopVR::Layer
