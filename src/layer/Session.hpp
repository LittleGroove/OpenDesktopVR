// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Everything the layer owns for the one session it attaches to (XR-18):
// the game's D3D11 device, our LOCAL and VIEW spaces, the frame composer,
// panels and stats. Created after the game's xrCreateSession succeeds;
// destroyed before the game's xrDestroySession runs.
#pragma once

#include "Dispatch.hpp"
#include "Hands.hpp"
#include "HostPanels.hpp"
#include "LayerStats.hpp"
#include "TestPanel.hpp"

#include <OpenDesktopVR/D3D11Util.hpp>
#include <OpenDesktopVR/Layer/Composition.hpp>
#include <OpenDesktopVR/Layer/LayerConfig.hpp>

#include <winrt/base.h>

#include <array>
#include <memory>
#include <mutex>
#include <optional>

namespace OpenDesktopVR::Layer {

struct InstanceState;

class Session {
 public:
  // Null if this session can't be composed into (unsupported graphics API,
  // missing functions): the caller then passes it through. Never throws.
  static std::unique_ptr<Session> Create(
    const InstanceState& instance,
    XrSession session,
    const XrSessionCreateInfo& info) noexcept;

  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  XrSession Handle() const noexcept {
    return mSession;
  }

  // The frame to hand to the next xrEndFrame: the game's own, or one with our
  // layers added. Never allocates in steady state, never throws. Any
  // unexpected error passes this and every later frame through unchanged.
  XrFrameEndInfo Compose(const XrFrameEndInfo& game) noexcept;

 private:
  Session(const InstanceState& instance, XrSession session);
  bool Initialize(const XrSessionCreateInfo& info);
  std::optional<XrPosef> LocateHead(XrTime time) noexcept;
  void PrepareReport() noexcept;

  static constexpr std::size_t MaxPanels = 16;

  const InstanceState& mInstance;
  const NextFunctions& mNext;
  const XrSession mSession;
  std::mutex mFrameMutex;// guards per-frame storage against concurrent misuse

  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext1> mContext;
  std::unique_ptr<D3D11Util::PrivateContextState> mContextState;
  XrSpace mLocalSpace {XR_NULL_HANDLE};
  XrSpace mViewSpace {XR_NULL_HANDLE};
  uint32_t mMaxLayers {16};
  DXGI_FORMAT mPanelFormat {DXGI_FORMAT_UNKNOWN};

  std::unique_ptr<FrameComposer> mComposer;
  std::array<ComposedPanel, MaxPanels> mPanels {};
  std::unique_ptr<TestPanel> mTestPanel;
  std::unique_ptr<HostPanels> mHostPanels;
  std::unique_ptr<Hands> mHands;
  SharedAbi::LayerStatus mReport {};
  TimeUtil::PeriodicTimer mReportTimer {std::chrono::seconds(1)};

  LayerStats mStats;
  bool mPassthrough {false};
  bool mLoggedFirstFrame {false};
};

// Per-instance state shared by the hooks
struct InstanceState {
  XrInstance mInstance {XR_NULL_HANDLE};
  NextFunctions mNext;
  LayerConfig mConfig;
  bool mPassthrough {false};
  bool mHandTrackingEnabled {false};
  std::string mApplicationName;
  std::string mRuntimeName;
};

}// namespace OpenDesktopVR::Layer
