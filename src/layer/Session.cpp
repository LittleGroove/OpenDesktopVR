// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "Session.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

#include <dxgi.h>

#include <algorithm>
#include <vector>

namespace OpenDesktopVR::Layer {

namespace {

std::string_view GraphicsApiName(XrStructureType type) {
  switch (type) {
    case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR:
      return "Direct3D 11";
    case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR:
      return "Direct3D 12";
    case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR:
      return "Vulkan";
    case XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR:
      return "OpenGL";
    default:
      return "an unknown graphics API";
  }
}

}// namespace

Session::Session(const InstanceState& instance, XrSession session)
  : mInstance(instance), mNext(instance.mNext), mSession(session) {
}

std::unique_ptr<Session> Session::Create(
  const InstanceState& instance,
  XrSession session,
  const XrSessionCreateInfo& info) noexcept {
  try {
    std::unique_ptr<Session> created {new Session(instance, session)};
    if (!created->Initialize(info)) {
      return nullptr;
    }
    return created;
  } catch (...) {
    ThreadUtil::ReportEscapedException("session setup");
    return nullptr;
  }
}

bool Session::Initialize(const XrSessionCreateInfo& info) {
  if (!mNext.mHasCore) {
    Log::Error("The next layer is missing functions the layer needs; passing this session through");
    return false;
  }

  // Graphics API: Direct3D 11 only in v1; anything else passes through
  const auto* binding = XrUtil::FindInChain<XrGraphicsBindingD3D11KHR>(info.next, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR);
  if (!binding || !binding->device) {
    const auto* first = static_cast<const XrBaseInStructure*>(info.next);
    Log::Warning(
      "The game uses {}; only Direct3D 11 is supported so far. Passing through",
      GraphicsApiName(first ? first->type : XR_TYPE_UNKNOWN));
    return false;
  }
  mDevice.copy_from(binding->device);
  winrt::com_ptr<ID3D11DeviceContext> context;
  mDevice->GetImmediateContext(context.put());
  if (!context || FAILED(context->QueryInterface(mContext.put()))) {
    Log::Error("No Direct3D 11.1 immediate context; passing through");
    return false;
  }
  mContextState = std::make_unique<D3D11Util::PrivateContextState>(mDevice.get());
  if (!mContextState->IsValid()) {
    Log::Warning("No private context state; copies only (they don't change pipeline state)");
  }

  // Layer limit (XR-9)
  XrSystemProperties system {XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(mNext.xrGetSystemProperties(mInstance.mInstance, info.systemId, &system))) {
    mMaxLayers = system.graphicsProperties.maxLayerCount;
    Log::Info("System '{}': up to {} composition layers", system.systemName, mMaxLayers);
  } else {
    Log::Warning("Couldn't read system properties; assuming 16 layers (the OpenXR minimum)");
  }

  // Panel format: the runtime's preferred 8-bit color format (sRGB first)
  uint32_t formatCount = 0;
  std::vector<int64_t> formats;
  if (XR_SUCCEEDED(mNext.xrEnumerateSwapchainFormats(mSession, 0, &formatCount, nullptr)) && formatCount > 0) {
    formats.resize(formatCount);
    if (XR_FAILED(mNext.xrEnumerateSwapchainFormats(mSession, formatCount, &formatCount, formats.data()))) {
      formats.clear();
    }
  }
  const auto format = D3D11Util::ChoosePanelFormat(formats);
  if (!format) {
    Log::Error("The runtime offers no 8-bit color swapchain format we can use; passing through");
    return false;
  }
  mPanelFormat = *format;
  Log::Info("Panel swapchain format: DXGI {}", static_cast<int>(mPanelFormat));

  // Our own spaces (doc 03 "Reference space")
  for (const auto& [type, space]: {
         std::pair {XR_REFERENCE_SPACE_TYPE_LOCAL, &mLocalSpace},
         std::pair {XR_REFERENCE_SPACE_TYPE_VIEW, &mViewSpace},
       }) {
    const XrReferenceSpaceCreateInfo spaceInfo {
      XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, type, XrUtil::IdentityPose};
    if (const auto result = mNext.xrCreateReferenceSpace(mSession, &spaceInfo, space); XR_FAILED(result)) {
      Log::Error("Couldn't create a reference space: {}; passing through", XrUtil::ResultName(result));
      return false;
    }
  }

  mComposer = std::make_unique<FrameComposer>(std::max<uint32_t>(mMaxLayers, 16) + 8, MaxPanels);

  // The host's panels, and the report the host reads (filled once here, so
  // the once-a-second report never allocates)
  mHostPanels = std::make_unique<HostPanels>(
    mNext, mSession, mLocalSpace, mDevice.get(), mContext.get(), *mContextState, mPanelFormat, mStats);
  if (!mHostPanels->IsUsable()) {
    Log::Warning("Host panels are unavailable on this device (shared fences need Windows 10 1703 or later)");
  }
  PrepareReport();

  if (mInstance.mConfig.mTestPanel) {
    mTestPanel = std::make_unique<TestPanel>(mNext, mSession, mLocalSpace, mDevice.get(), mPanelFormat);
    Log::Info("The built-in test panel is on");
  }
  return true;
}

Session::~Session() {
  mStats.Write("Layer stats (session end, since the last line)");
  Log::Info(
    "Session ended after {} frames; layer CPU time per frame averaged {:.4f} ms",
    mStats.TotalFrames(),
    mStats.AverageCpuMs());
  // Children before the session itself; the game destroys the session after
  mHostPanels.reset();
  mTestPanel.reset();
  for (const auto space: {mLocalSpace, mViewSpace}) {
    if (space != XR_NULL_HANDLE) {
      mNext.xrDestroySpace(space);
    }
  }
}

void Session::PrepareReport() noexcept {
  auto& report = mReport;
  report = {};
  StringUtil::CopyTruncated(StringUtil::Narrow(ProcessUtil::ExecutablePath().filename().wstring()), report.mExecutable);
  StringUtil::CopyTruncated(mInstance.mApplicationName, report.mApplication);
  StringUtil::CopyTruncated(mInstance.mRuntimeName, report.mRuntime);
  report.mGraphicsApi = static_cast<uint32_t>(SharedAbi::GraphicsApi::D3D11);
  report.mMaxLayerCount = mMaxLayers;
  report.mHandTrackingAvailable = mInstance.mHandTrackingEnabled ? 1 : 0;
  // The host renders on the game's adapter, or shared textures won't open
  winrt::com_ptr<IDXGIDevice> dxgiDevice;
  winrt::com_ptr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc {};
  if (
    SUCCEEDED(mDevice->QueryInterface(dxgiDevice.put())) && SUCCEEDED(dxgiDevice->GetAdapter(adapter.put()))
    && SUCCEEDED(adapter->GetDesc(&desc))) {
    report.mAdapterLuidLow = desc.AdapterLuid.LowPart;
    report.mAdapterLuidHigh = desc.AdapterLuid.HighPart;
  }
}

std::optional<XrPosef> Session::LocateHead(XrTime time) noexcept {
  XrSpaceLocation location {XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(mNext.xrLocateSpace(mViewSpace, mLocalSpace, time, &location)) || !XrUtil::IsUsable(location)) {
    return std::nullopt;
  }
  return location.pose;
}

XrFrameEndInfo Session::Compose(const XrFrameEndInfo& game) noexcept {
  std::scoped_lock lock(mFrameMutex);
  const auto start = TimeUtil::Now();
  XrFrameEndInfo frame = game;
  uint32_t panelsComposed = 0;
  uint32_t panelsDropped = 0;
  FrameCosts costs;
  const bool zeroLayers = game.layerCount == 0 || !game.layers;

  // XR-6: frames with no game layers pass through untouched
  if (!mPassthrough && !zeroLayers) {
    try {
      std::size_t panelCount = 0;
      if (mHostPanels) {
        panelCount += mHostPanels->Compose(start, {mPanels.data(), mPanels.size()});
        costs = mHostPanels->LastFrameCosts();
        const bool reportNow = mHostPanels->TakeReportDue();
        if (mReportTimer.Due(start) || reportNow) {
          mReport.mFramesSinceAttach = mStats.TotalFrames();
          mHostPanels->Report(mReport);
        }
      }
      std::optional<XrPosef> head;
      if (mTestPanel && mTestPanel->NeedsHeadPose()) {
        head = LocateHead(game.displayTime);
      }
      if (mTestPanel) {
        if (mTestPanel->Prepare(head)) {
          const auto scope = mContextState->Enter(mContext.get());
          if (mTestPanel->Draw(mContext.get())) {
            mStats.ImageReleased();
          }
        }
        if (const auto* quad = mTestPanel->Quad(); quad && panelCount < mPanels.size()) {
          mPanels[panelCount++] = {quad, 0, false};
        }
      }
      // Distances only matter when there's an order to decide
      if (panelCount > 1) {
        if (const auto now = LocateHead(game.displayTime)) {
          for (std::size_t i = 0; i < panelCount; ++i) {
            mPanels[i].mDistance
              = XrUtil::DistanceToRect(mPanels[i].mQuad->pose, mPanels[i].mQuad->size, now->position);
          }
        }
      }
      const auto result = mComposer->Compose(
        {game.layers, game.layerCount}, {mPanels.data(), panelCount}, nullptr, mMaxLayers);
      frame.layers = result.mLayers.data();
      frame.layerCount = static_cast<uint32_t>(result.mLayers.size());
      panelsComposed = result.mPanelsComposed;
      panelsDropped = result.mPanelsDropped;
      if (!mLoggedFirstFrame && panelsComposed > 0) {
        mLoggedFirstFrame = true;
        Log::Info("First frame with panels: {} game layer(s) + {} panel(s)", game.layerCount, panelsComposed);
      }
    } catch (...) {
      // Only the core composition path gets here: pass through from now on
      mPassthrough = true;
      ThreadUtil::ReportEscapedException("frame composition; passing through for the rest of the session");
      frame = game;
      panelsComposed = 0;
    }
  }

  mStats.FrameDone(TimeUtil::ToMilliseconds(TimeUtil::Now() - start), zeroLayers, panelsComposed, panelsDropped, costs);
  mStats.MaybeLog();
  return frame;
}

}// namespace OpenDesktopVR::Layer
