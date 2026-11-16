// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "Hands.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace OpenDesktopVR::Layer {

namespace {

using HandInteraction::JointLocation;
using HandInteraction::Quaternion;
using HandInteraction::Vector3;

constexpr XrSpaceLocationFlags BothValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

XrPosef ToXr(const JointLocation& joint) noexcept {
  return {
    {joint.mOrientation.x, joint.mOrientation.y, joint.mOrientation.z, joint.mOrientation.w},
    {joint.mPosition.x, joint.mPosition.y, joint.mPosition.z},
  };
}

void FromXr(const XrPosef& pose, float radius, JointLocation& joint) noexcept {
  joint.mPosition = {pose.position.x, pose.position.y, pose.position.z};
  joint.mOrientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
  joint.mRadius = radius;
}

D3D11_RECT ToD3D(const PixelRect& rect, int32_t offsetX) noexcept {
  return {rect.mLeft + offsetX, rect.mTop, rect.mRight + offsetX, rect.mBottom};
}

float Visibility(float value) noexcept {
  // From another process: untrusted (TH-9)
  return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

}// namespace

Hands::Hands(
  const NextFunctions& next,
  XrSession session,
  XrSpace localSpace,
  ID3D11Device* device,
  ID3D11DeviceContext1* context,
  const D3D11Util::PrivateContextState& contextState,
  DXGI_FORMAT format,
  bool systemSupportsHands,
  LayerStats& stats) noexcept
  : mNext(next),
    mSession(session),
    mLocalSpace(localSpace),
    mDevice(device),
    mContext(context),
    mContextState(contextState),
    mFormat(format),
    mSystemSupportsHands(systemSupportsHands && next.mHasHandTracking),
    mStats(stats) {
}

Hands::~Hands() {
  DestroySwapchain();
  for (auto& tracker: mTrackers) {
    if (tracker != XR_NULL_HANDLE) {
      mNext.xrDestroyHandTrackerEXT(tracker);
      tracker = XR_NULL_HANDLE;
    }
  }
}

bool Hands::EnsureTrackers() noexcept {
  if (mTrackers[0] != XR_NULL_HANDLE && mTrackers[1] != XR_NULL_HANDLE) {
    return true;
  }
  if (mTrackingFailed) {
    return false;
  }
  if (!mSystemSupportsHands) {
    mTrackingFailed = true;
    Log::Info("Hands: this runtime or headset has no hand tracking; panels work without it");
    return false;
  }
  for (const auto hand: {XR_HAND_LEFT_EXT, XR_HAND_RIGHT_EXT}) {
    const XrHandTrackerCreateInfoEXT info {
      XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, nullptr, hand, XR_HAND_JOINT_SET_DEFAULT_EXT};
    auto& tracker = mTrackers[hand == XR_HAND_LEFT_EXT ? 0 : 1];
    if (const auto result = mNext.xrCreateHandTrackerEXT(mSession, &info, &tracker); XR_FAILED(result)) {
      // XR-2: hands off for this session; never retried per frame
      Log::Warning("Hands: the runtime refused a hand tracker ({}); hands are off for this session", XrUtil::ResultName(result));
      tracker = XR_NULL_HANDLE;
      mTrackingFailed = true;
      return false;
    }
  }
  Log::Info("Hands: tracking both hands");
  return true;
}

bool Hands::WantsHead(HostLink::Client& link) noexcept {
  return !mTrackingFailed && link.OwnsStatus() && (link.ReadHandState().mFlags & SharedAbi::HandFlags::WantsSamples);
}

void Hands::Locate(XrTime time) noexcept {
  for (std::size_t h = 0; h < 2; ++h) {
    auto& out = mSample.mHands[h];
    auto& drawn = mDrawn[h].mJoints;
    out = {};
    drawn.mIsTracked = false;
    XrHandJointLocationsEXT locations {
      XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_FALSE, SharedAbi::HandJointCount, mLocations.data()};
    const XrHandJointsLocateInfoEXT info {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, mLocalSpace, time};
    if (XR_FAILED(mNext.xrLocateHandJointsEXT(mTrackers[h], &info, &locations)) || !locations.isActive) {
      continue;
    }
    // XR-10: the whole hand, or nothing
    const bool valid = std::all_of(mLocations.begin(), mLocations.end(), [](const XrHandJointLocationEXT& joint) {
      return (joint.locationFlags & BothValid) == BothValid && XrUtil::IsFinite(joint.pose) && std::isfinite(joint.radius)
        && joint.radius >= 0;
    });
    if (!valid) {
      continue;
    }
    for (std::size_t j = 0; j < SharedAbi::HandJointCount; ++j) {
      const auto& location = mLocations[j];
      auto& joint = out.mJoints[j];
      const auto& p = location.pose;
      const float pose[7] {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
      std::copy(std::begin(pose), std::end(pose), joint.mPose);
      joint.mRadius = location.radius;
      FromXr(p, location.radius, drawn.mJoints[j]);
    }
    out.mTracked = 1;
    drawn.mIsTracked = true;
  }
}

const XrCompositionLayerBaseHeader* Hands::Frame(const FrameInput& input, HostLink::Client& link, double& coverage) noexcept {
  coverage = 0;
  mDrawn[0].mVisibility = mDrawn[1].mVisibility = 0;
  const auto state = link.ReadHandState();
  // Hands off in the host, or another game's layer owns the link
  if (!(state.mFlags & SharedAbi::HandFlags::WantsSamples) || !link.OwnsStatus()) {
    return nullptr;
  }
  // No panels: no samples; the host times out and lets go (XR-19)
  if (input.mPanels.empty() || !EnsureTrackers()) {
    return nullptr;
  }
  Locate(input.mDisplayTime);
  mSample.mDisplayTime = input.mDisplayTime;
  mSample.mFlags = 0;
  if (input.mHead) {
    const auto& p = *input.mHead;
    const float head[7] {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    std::copy(std::begin(head), std::end(head), mSample.mHead);
    mSample.mFlags |= SharedAbi::SampleFlags::HeadValid;
  }
  link.WriteTrackingSample(mSample);

  for (std::size_t h = 0; h < 2; ++h) {
    mDrawn[h].mVisibility = mDrawn[h].mJoints.mIsTracked ? Visibility(state.mVisibility[h]) : 0.0f;
  }
  if (mDrawingFailed || !D3D11::HandRenderer::AnyVisible(mDrawn)) {
    return nullptr;
  }
  return Draw(input, coverage);
}

void Hands::DestroySwapchain() noexcept {
  mTargets.clear();
  mImageAreas.clear();
  if (mSwapchain != XR_NULL_HANDLE) {
    mNext.xrDestroySwapchain(mSwapchain);
    mSwapchain = XR_NULL_HANDLE;
  }
}

bool Hands::EnsureSwapchain(uint32_t cellWidth, uint32_t cellHeight, uint32_t views) noexcept {
  if (mSwapchain != XR_NULL_HANDLE && !mNeedsRecreate && cellWidth == mCellWidth && cellHeight == mCellHeight && views == mViews) {
    return true;
  }
  DestroySwapchain();
  mNeedsRecreate = false;
  mCellWidth = cellWidth;
  mCellHeight = cellHeight;
  mViews = views;
  // One image, the eyes side by side, each a whole cell
  const XrSwapchainCreateInfo info {
    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
    .format = static_cast<int64_t>(mFormat),
    .sampleCount = 1,
    .width = cellWidth * views,
    .height = cellHeight,
    .faceCount = 1,
    .arraySize = 1,
    .mipCount = 1,
  };
  if (const auto result = mNext.xrCreateSwapchain(mSession, &info, &mSwapchain); XR_FAILED(result)) {
    Log::Error("Hands: the hand layer's swapchain ({}x{}) couldn't be created: {}; hands aren't drawn this session",
      info.width, info.height, XrUtil::ResultName(result));
    mSwapchain = XR_NULL_HANDLE;
    mDrawingFailed = true;
    return false;
  }
  try {
    uint32_t count = 0;
    if (XR_FAILED(mNext.xrEnumerateSwapchainImages(mSwapchain, 0, &count, nullptr)) || count == 0) {
      throw std::runtime_error("no images");
    }
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(mNext.xrEnumerateSwapchainImages(mSwapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
      throw std::runtime_error("no images");
    }
    mTargets.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const D3D11_RENDER_TARGET_VIEW_DESC desc {.Format = mFormat, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D};
      winrt::check_hresult(mDevice->CreateRenderTargetView(images[i].texture, &desc, mTargets[i].put()));
    }
    // A new swapchain's images hold anything: each counts as fully drawn
    std::array<PixelRect, MaxViews> whole {};
    for (uint32_t v = 0; v < views; ++v) {
      whole[v] = {0, 0, static_cast<int32_t>(cellWidth), static_cast<int32_t>(cellHeight)};
    }
    mImageAreas.assign(count, whole);
  } catch (...) {
    Log::Error("Hands: the hand layer's images couldn't be used; hands aren't drawn this session");
    DestroySwapchain();
    mDrawingFailed = true;
    return false;
  }
  Log::Info("Hands: hand layer {}x{} per eye, {} eyes", cellWidth, cellHeight, views);
  return true;
}

const XrCompositionLayerBaseHeader* Hands::Draw(const FrameInput& input, double& coverage) noexcept {
  // The game's projection layer: the first with 1-4 views
  const XrCompositionLayerProjection* game = nullptr;
  for (const auto* layer: input.mGameLayers) {
    if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
      if (projection->viewCount >= 1 && projection->viewCount <= MaxViews && projection->views) {
        game = projection;
        break;
      }
    }
  }
  if (!game) {
    return nullptr;
  }

  // Touch limiting: drawn hands stop at the surface of a panel they poke
  // into (doc 05); interaction uses where they really are
  if (input.mHead) {
    std::size_t panelCount = 0;
    for (const auto& composed: input.mPanels) {
      if (!composed.mQuad || panelCount >= mPanels.size()) {
        continue;
      }
      const auto& quad = *composed.mQuad;
      mPanels[panelCount++] = {
        .mPosition = {quad.pose.position.x, quad.pose.position.y, quad.pose.position.z},
        .mOrientation = {quad.pose.orientation.x, quad.pose.orientation.y, quad.pose.orientation.z, quad.pose.orientation.w},
        .mSize = {quad.size.width, quad.size.height},
      };
    }
    const Vector3 viewer {input.mHead->position.x, input.mHead->position.y, input.mHead->position.z};
    for (auto& hand: mDrawn) {
      if (hand.mVisibility > 0 && hand.mJoints.mIsTracked) {
        hand.mJoints = HandInteraction::LimitToSurfaces(hand.mJoints, {mPanels.data(), panelCount}, std::nullopt, viewer);
      }
    }
  }

  // XR-12: into the game layer's space (STAGE or LOCAL)
  XrSpaceLocation located {XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(mNext.xrLocateSpace(mLocalSpace, game->space, input.mDisplayTime, &located)) || !XrUtil::IsUsable(located)) {
    return nullptr;
  }
  std::size_t sphereCount = 0;
  for (auto& hand: mDrawn) {
    if (hand.mVisibility <= 0 || !hand.mJoints.mIsTracked) {
      continue;
    }
    for (auto& joint: hand.mJoints.mJoints) {
      FromXr(XrUtil::Compose(located.pose, ToXr(joint)), joint.mRadius, joint);
      mSpheres[sphereCount++] = {{joint.mPosition.x, joint.mPosition.y, joint.mPosition.z}, joint.mRadius};
    }
  }

  // Each eye's cell: the game's image size, capped
  const auto& extent = game->views[0].subImage.imageRect.extent;
  uint32_t cellWidth = extent.width > 0 ? static_cast<uint32_t>(extent.width) : 1024;
  uint32_t cellHeight = extent.height > 0 ? static_cast<uint32_t>(extent.height) : 1024;
  if (const auto longest = std::max(cellWidth, cellHeight); longest > MaxCellSize) {
    cellWidth = std::max(1u, cellWidth * MaxCellSize / longest);
    cellHeight = std::max(1u, cellHeight * MaxCellSize / longest);
  }
  const uint32_t views = game->viewCount;

  // XR-13, XR-14: where the hands are in each eye; nothing anywhere, no layer
  std::array<PixelRect, MaxViews> areas {};
  bool any = false;
  for (uint32_t v = 0; v < views; ++v) {
    const auto& view = game->views[v];
    areas[v] = SpheresInView(view.pose, view.fov, cellWidth, cellHeight, {mSpheres.data(), sphereCount}, 4);
    any = any || !areas[v].IsEmpty();
  }
  if (!any || !EnsureSwapchain(cellWidth, cellHeight, views)) {
    return nullptr;
  }

  uint32_t index = 0;
  const XrSwapchainImageAcquireInfo acquireInfo {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(mNext.xrAcquireSwapchainImage(mSwapchain, &acquireInfo, &index)) || index >= mTargets.size()) {
    mNeedsRecreate = true;
    return nullptr;
  }
  const XrSwapchainImageWaitInfo waitInfo {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 100'000'000};
  if (const auto waited = mNext.xrWaitSwapchainImage(mSwapchain, &waitInfo); XR_FAILED(waited) || waited == XR_TIMEOUT_EXPIRED) {
    // XR-15: the acquired image can't be released now; re-create next frame
    Log::Warning("Hands: hand layer image wait failed ({}); re-creating it", XrUtil::ResultName(waited));
    mNeedsRecreate = true;
    return nullptr;
  }
  bool drawn = true;
  {
    const auto scope = mContextState.Enter(mContext);
    try {
      if (!mRenderer) {
        mRenderer = std::make_unique<D3D11::HandRenderer>(mDevice);
      }
      auto* target = mTargets[index].get();
      auto& imageAreas = mImageAreas[index];
      const float transparent[4] {0, 0, 0, 0};
      for (uint32_t v = 0; v < views; ++v) {
        const auto offset = static_cast<int32_t>(v * cellWidth);
        // Clear what this image held there before, and where the hands go now
        const auto clear = Pad(Union(imageAreas[v], areas[v]), 2, cellWidth, cellHeight);
        if (!clear.IsEmpty()) {
          const auto rect = ToD3D(clear, offset);
          mContext->ClearView(target, transparent, &rect, 1);
        }
        imageAreas[v] = areas[v];
        if (areas[v].IsEmpty()) {
          continue;
        }
        const auto& view = game->views[v];
        const D3D11_VIEWPORT viewport {
          static_cast<float>(offset), 0, static_cast<float>(cellWidth), static_cast<float>(cellHeight), 0, 1};
        const D3D11::HandRenderer::Eye eye {
          .mPosition = {view.pose.position.x, view.pose.position.y, view.pose.position.z},
          .mOrientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w},
          .mTanLeft = std::tan(view.fov.angleLeft),
          .mTanRight = std::tan(view.fov.angleRight),
          .mTanUp = std::tan(view.fov.angleUp),
          .mTanDown = std::tan(view.fov.angleDown),
        };
        mRenderer->Render(mContext, target, viewport, ToD3D(areas[v], offset), eye, mDrawn);
      }
    } catch (...) {
      ThreadUtil::ReportEscapedException("drawing hands; hands aren't drawn this session");
      mDrawingFailed = true;
      drawn = false;
    }
  }
  const XrSwapchainImageReleaseInfo releaseInfo {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  if (XR_FAILED(mNext.xrReleaseSwapchainImage(mSwapchain, &releaseInfo))) {
    mNeedsRecreate = true;
    return nullptr;
  }
  if (!drawn) {
    return nullptr;
  }
  mStats.ImageReleased();

  // XR-11: exactly the game's views, each over its whole cell
  for (uint32_t v = 0; v < views; ++v) {
    const auto& view = game->views[v];
    mViewsOut[v] = {
      .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
      .pose = view.pose,
      .fov = view.fov,
      .subImage = {
        .swapchain = mSwapchain,
        .imageRect = {{static_cast<int32_t>(v * cellWidth), 0}, {static_cast<int32_t>(cellWidth), static_cast<int32_t>(cellHeight)}},
        .imageArrayIndex = 0,
      },
    };
    coverage += static_cast<double>(areas[v].Area()) / (static_cast<double>(cellWidth) * cellHeight);
  }
  coverage /= views;
  mLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  mLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mLayer.space = game->space;
  mLayer.viewCount = views;
  mLayer.views = mViewsOut.data();
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mLayer);
}

}// namespace OpenDesktopVR::Layer
