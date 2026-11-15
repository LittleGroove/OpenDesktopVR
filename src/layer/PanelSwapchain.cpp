// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "PanelSwapchain.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

namespace OpenDesktopVR::Layer {

PanelSwapchain::PanelSwapchain(const NextFunctions& next, XrSession session, DXGI_FORMAT format) noexcept
  : mNext(next), mSession(session), mFormat(format) {
}

PanelSwapchain::~PanelSwapchain() {
  Destroy();
}

void PanelSwapchain::Destroy() noexcept {
  mImages.clear();
  if (mSwapchain != XR_NULL_HANDLE) {
    mNext.xrDestroySwapchain(mSwapchain);
    mSwapchain = XR_NULL_HANDLE;
  }
  mShownKey = 0;
}

bool PanelSwapchain::Ensure(uint32_t width, uint32_t height) noexcept {
  if (mSwapchain != XR_NULL_HANDLE && !mNeedsRecreate && width == mWidth && height == mHeight) {
    return true;
  }
  Destroy();
  mNeedsRecreate = false;
  mWidth = width;
  mHeight = height;
  // Never clamped to the runtime's "maximum" swapchain size (XR-16)
  const XrSwapchainCreateInfo info {
    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
    .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
      | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
    .format = static_cast<int64_t>(mFormat),
    .sampleCount = 1,
    .width = width,
    .height = height,
    .faceCount = 1,
    .arraySize = 1,
    .mipCount = 1,
  };
  if (const auto result = mNext.xrCreateSwapchain(mSession, &info, &mSwapchain); XR_FAILED(result)) {
    Log::Error("Panel swapchain {}x{} creation failed: {}", width, height, XrUtil::ResultName(result));
    mSwapchain = XR_NULL_HANDLE;
    return false;
  }
  try {
    uint32_t count = 0;
    if (XR_FAILED(mNext.xrEnumerateSwapchainImages(mSwapchain, 0, &count, nullptr)) || count == 0) {
      Destroy();
      return false;
    }
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(mNext.xrEnumerateSwapchainImages(
          mSwapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
      Destroy();
      return false;
    }
    mImages.reserve(count);
    for (const auto& image: images) {
      winrt::com_ptr<ID3D11Texture2D> texture;
      texture.copy_from(image.texture);
      mImages.push_back(std::move(texture));
    }
  } catch (...) {
    Destroy();
    return false;
  }
  return true;
}

bool PanelSwapchain::Present(ID3D11DeviceContext* context, ID3D11Texture2D* source, uint64_t contentKey) noexcept {
  if (mSwapchain == XR_NULL_HANDLE || !context || !source) {
    return false;
  }
  mShownKey = 0;// a failed draw never counts as shown (SY-4)
  uint32_t index = 0;
  const XrSwapchainImageAcquireInfo acquireInfo {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(mNext.xrAcquireSwapchainImage(mSwapchain, &acquireInfo, &index)) || index >= mImages.size()) {
    mNeedsRecreate = true;
    return false;
  }
  // The runtime should have the image ready at once; a short timeout is a
  // safety net, not a wait we expect to hit
  const XrSwapchainImageWaitInfo waitInfo {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 100'000'000};
  if (const auto waited = mNext.xrWaitSwapchainImage(mSwapchain, &waitInfo); XR_FAILED(waited) || waited == XR_TIMEOUT_EXPIRED) {
    // The acquired image can't be released now: re-create next frame (XR-15)
    Log::Warning("Panel swapchain image wait failed ({}); re-creating it", XrUtil::ResultName(waited));
    mNeedsRecreate = true;
    return false;
  }
  const D3D11_BOX box {0, 0, 0, mWidth, mHeight, 1};
  context->CopySubresourceRegion(mImages[index].get(), 0, 0, 0, 0, source, 0, &box);
  const XrSwapchainImageReleaseInfo releaseInfo {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  if (XR_FAILED(mNext.xrReleaseSwapchainImage(mSwapchain, &releaseInfo))) {
    mNeedsRecreate = true;
    return false;
  }
  ++mImagesReleased;
  mShownKey = contentKey;
  return true;
}

}// namespace OpenDesktopVR::Layer
