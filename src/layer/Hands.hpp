// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Hands inside the game (doc 03 steps 2 and 4, doc 05 "Hand visual"):
// - Tracking: once the host asks for samples, one hand tracker per hand
//   (created once; a refusal turns hands off for the session, XR-2). While
//   panels are composed, each frame's joints (LOCAL space, at the frame's
//   display time) go to the host with the head pose. A hand with any
//   invalid or non-finite joint is untracked for that frame (XR-10).
// - Drawing: the host says how much of each hand to show (its pointing
//   pose, the user's settings). The hand is drawn where it is this frame,
//   pulled back to the surface of a panel it pokes into (touch limiting,
//   for drawing only), into our own projection layer:
//   - in the game's space, with the game's exact eye poses and fields of
//     view, each eye's image rectangle a whole cell (XR-11, XR-12);
//   - drawing and clearing only around the hands, with the area each
//     swapchain image last held tracked per image (XR-13);
//   - not submitted when no hand is in view (XR-14);
//   - a failed image wait re-creates the swapchain next frame (XR-15);
//   - inside the layer's private D3D11 state (XR-20).
// Never throws; allocates only when it creates the swapchain.
#pragma once

#include "Dispatch.hpp"
#include "LayerStats.hpp"

#include <OpenDesktopVR/D3D11/HandRenderer.hpp>
#include <OpenDesktopVR/D3D11Util.hpp>
#include <OpenDesktopVR/HandInteraction.hpp>
#include <OpenDesktopVR/HostLink.hpp>
#include <OpenDesktopVR/Layer/Composition.hpp>
#include <OpenDesktopVR/Layer/HandBounds.hpp>

#include <winrt/base.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace OpenDesktopVR::Layer {

class Hands {
 public:
  static constexpr uint32_t MaxViews = 4;
  // The hand layer's image per eye is at most this many pixels on its long
  // side (the game's own size if smaller): see-through hands don't need
  // the game's full resolution
  static constexpr uint32_t MaxCellSize = 1200;

  Hands(
    const NextFunctions& next,
    XrSession session,
    const XrSpace& localSpace,
    ID3D11Device* device,
    ID3D11DeviceContext1* context,
    const D3D11Util::PrivateContextState& contextState,
    DXGI_FORMAT format,
    bool systemSupportsHands,
    LayerStats& stats) noexcept;
  ~Hands();
  Hands(const Hands&) = delete;
  Hands& operator=(const Hands&) = delete;

  struct FrameInput {
    XrTime mDisplayTime {0};
    std::span<const XrCompositionLayerBaseHeader* const> mGameLayers;
    // The panels composed this frame (their quads are in LOCAL space)
    std::span<const ComposedPanel> mPanels;
    std::optional<XrPosef> mHead;// in LOCAL space
  };

  // True if this frame needs the head pose (a sample goes to the host)
  bool WantsHead(HostLink::Client& link) noexcept;

  // Tracks, tells the host, and draws. Returns the hand layer to compose
  // last, or null. `coverage` is the share of each eye's view it covers.
  const XrCompositionLayerBaseHeader* Frame(const FrameInput& input, HostLink::Client& link, double& coverage) noexcept;

 private:
  bool EnsureTrackers() noexcept;
  void Locate(XrTime time) noexcept;
  bool EnsureSwapchain(uint32_t cellWidth, uint32_t cellHeight, uint32_t views) noexcept;
  void DestroySwapchain() noexcept;
  const XrCompositionLayerBaseHeader* Draw(const FrameInput& input, double& coverage) noexcept;

  const NextFunctions& mNext;
  XrSession mSession;
  // The session's panel space: re-made at each recenter (ADR-037)
  const XrSpace& mLocalSpace;
  ID3D11Device* mDevice;
  ID3D11DeviceContext1* mContext;
  const D3D11Util::PrivateContextState& mContextState;
  DXGI_FORMAT mFormat;
  bool mSystemSupportsHands;
  LayerStats& mStats;

  // Tracking
  std::array<XrHandTrackerEXT, 2> mTrackers {XR_NULL_HANDLE, XR_NULL_HANDLE};
  bool mTrackingFailed {false};
  std::array<XrHandJointLocationEXT, SharedAbi::HandJointCount> mLocations {};
  SharedAbi::TrackingSample mSample {};
  std::array<HandInteraction::RenderHand, 2> mDrawn {};

  // Drawing
  std::unique_ptr<D3D11::HandRenderer> mRenderer;
  bool mDrawingFailed {false};
  XrSwapchain mSwapchain {XR_NULL_HANDLE};
  std::vector<winrt::com_ptr<ID3D11RenderTargetView>> mTargets;
  // Per swapchain image, per eye: the area last drawn into it
  std::vector<std::array<PixelRect, MaxViews>> mImageAreas;
  uint32_t mCellWidth {0};
  uint32_t mCellHeight {0};
  uint32_t mViews {0};
  bool mNeedsRecreate {false};
  std::array<XrCompositionLayerProjectionView, MaxViews> mViewsOut {};
  XrCompositionLayerProjection mLayer {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<HandInteraction::Panel, SharedAbi::MaxPanels> mPanels {};
  std::array<Sphere, 2 * SharedAbi::HandJointCount> mSpheres {};
};

}// namespace OpenDesktopVR::Layer
