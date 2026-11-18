// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
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
#include <OpenDesktopVR/Layer/LayerChain.hpp>
#include <OpenDesktopVR/Layer/LayerConfig.hpp>

#include <winrt/base.h>

#include <array>
#include <atomic>
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

  // The runtime is moving LOCAL (a recenter: XrEventDataReferenceSpaceChangePending).
  // From the first frame at or after `changeTime`, panels go where the head
  // is then. Any thread; never waits.
  void RecenterAt(XrTime changeTime) noexcept;

 private:
  Session(const InstanceState& instance, XrSession session);
  bool Initialize(const XrSessionCreateInfo& info);
  std::optional<XrPosef> LocateHead(XrTime time) noexcept;
  // A recenter asked for (the runtime's or the host's keybind), once its
  // time has come: the panel space is re-made at the head, level
  void ApplyRecenter(XrTime displayTime) noexcept;
  void PrepareReport() noexcept;

  static constexpr std::size_t MaxPanels = 16;

  const InstanceState& mInstance;
  const NextFunctions& mNext;
  const XrSession mSession;
  std::mutex mFrameMutex;// guards per-frame storage against concurrent misuse

  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext1> mContext;
  std::unique_ptr<D3D11Util::PrivateContextState> mContextState;
  // The panel space (doc 03 "Reference space"; ADR-037): LOCAL at first,
  // then, after each recenter, LOCAL moved to where the head was then
  // (level). Panels, hand samples and the head are all in it; the parts
  // hold it by reference, so a recenter re-makes it for them all.
  XrSpace mLocalSpace {XR_NULL_HANDLE};
  XrSpace mRawLocalSpace {XR_NULL_HANDLE};// plain LOCAL, to find the head in
  XrSpace mViewSpace {XR_NULL_HANDLE};
  // XrTime from which a recenter is due (0: none); set from any thread
  std::atomic<int64_t> mRecenterAt {0};
  const char* mRecenterWhy {"the runtime's recenter"};
  bool mLoggedStartHead {false};
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
  // For the host's diagnostics (M6c; CMP-1, CMP-2, CMP-7)
  ChainSummary mChain;
  bool mDepthEnabled {false};
};

}// namespace OpenDesktopVR::Layer
