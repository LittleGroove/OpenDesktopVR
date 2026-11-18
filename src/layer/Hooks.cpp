// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The layer's entry points: loader negotiation, dispatch, and the hooked
// OpenXR functions. Written from the OpenXR specification and the loader's
// "API Layer Interaction" documentation.
//
// Every hook catches everything (XR-1); our own work is guarded separately
// from the call to the next layer, so a failure in it passes the call through
// unchanged rather than failing the game's call.

#include "Dispatch.hpp"
#include "Session.hpp"

#include <OpenDesktopVR/Layer/ExtensionPolicy.hpp>
#include <OpenDesktopVR/Layer/LayerConfig.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Layer {
namespace {

// ---- Process-wide state ------------------------------------------------------
//
// Never destroyed: no cleanup at process exit (XR-17). No thread-local state
// between calls (XR-5).

struct Globals {
  std::shared_mutex mMutex;
  std::vector<std::unique_ptr<InstanceState>> mInstances;
  std::vector<std::pair<XrSession, InstanceState*>> mSessions;
  std::unique_ptr<Session> mAttached;// XR-18: one per process
  bool mLoggedSecondSession {false};
  // XR-8: the next layer's lookup, remembered from the last instance
  std::atomic<PFN_xrGetInstanceProcAddr> mLastNextGetInstanceProcAddr {nullptr};
  std::once_flag mStartOnce;
};

Globals& G() {
  static Globals* sGlobals = new Globals();
  return *sGlobals;
}

InstanceState* FindInstance(XrInstance instance) {
  for (const auto& state: G().mInstances) {
    if (state->mInstance == instance) {
      return state.get();
    }
  }
  return nullptr;
}

InstanceState* FindInstanceOfSession(XrSession session) {
  for (const auto& [handle, state]: G().mSessions) {
    if (handle == session) {
      return state;
    }
  }
  return nullptr;
}

// Logging starts with the first instance, never in DllMain. The DLL is pinned
// so the log thread never runs code that has been unloaded.
void StartOnce() {
  std::call_once(G().mStartOnce, [] {
    HMODULE self = nullptr;
    GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&StartOnce),
      &self);
    const auto name = ProcessUtil::ExecutableStem();
    Log::Config config;
    if (const auto folder = Product::LogsFolder(); !folder.empty()) {
      config.mFile = folder / (L"layer-" + (name.empty() ? std::wstring {L"unknown"} : name) + L".log");
    }
    Log::Start(config);
    Log::Info("Layer {} loaded into {}", Product::LayerName, StringUtil::Narrow(ProcessUtil::ExecutablePath().wstring()));
    Log::Info("Build: {}", Product::BuildDescription());
  });
}

// ---- Hooks ---------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction*);

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance) {
  PFN_xrDestroyInstance next = nullptr;
  try {
    // Declared in this order so the session (which refers to the instance
    // state) is destroyed first, even when unwinding
    std::unique_ptr<InstanceState> removed;
    std::unique_ptr<Session> attached;
    {
      std::unique_lock lock(G().mMutex);
      auto& instances = G().mInstances;
      for (auto it = instances.begin(); it != instances.end(); ++it) {
        if ((*it)->mInstance == instance) {
          removed = std::move(*it);
          instances.erase(it);
          break;
        }
      }
      if (!removed) {
        return XR_ERROR_HANDLE_INVALID;
      }
      std::erase_if(G().mSessions, [&](const auto& entry) { return entry.second == removed.get(); });
      // The attached session belonged to this instance if it's no longer mapped
      if (G().mAttached && !FindInstanceOfSession(G().mAttached->Handle())) {
        attached = std::move(G().mAttached);
      }
    }
    // Our session resources go before the instance that owns them
    attached.reset();
    next = removed->mNext.xrDestroyInstance;
    const auto result = next ? next(instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
    Log::Info("Instance destroyed: {}", XrUtil::ResultName(result));
    return result;
  } catch (...) {
    ThreadUtil::ReportEscapedException("xrDestroyInstance");
    return next ? next(instance) : XR_ERROR_RUNTIME_FAILURE;
  }
}

XRAPI_ATTR XrResult XRAPI_CALL
Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
  InstanceState* state = nullptr;
  {
    std::shared_lock lock(G().mMutex);
    state = FindInstance(instance);
  }
  if (!state || !state->mNext.xrCreateSession) {
    return XR_ERROR_HANDLE_INVALID;
  }
  const auto result = state->mNext.xrCreateSession(instance, info, session);
  if (XR_FAILED(result) || !info || !session) {
    return result;
  }
  ThreadUtil::CatchAll("xrCreateSession", [&] {
    bool attach = false;
    {
      std::unique_lock lock(G().mMutex);
      G().mSessions.emplace_back(*session, state);
      attach = !G().mAttached;
      if (!attach && !G().mLoggedSecondSession) {
        G().mLoggedSecondSession = true;
        Log::Warning("A second session in the same process is passed through (one session per process)");
      }
    }
    if (!attach) {
      return;
    }
    // Set up outside our lock: this calls into the runtime
    auto created = Session::Create(*state, *session, *info);
    std::unique_lock lock(G().mMutex);
    if (!G().mAttached && created) {
      G().mAttached = std::move(created);
      Log::Info("Attached to the game's session");
    }
  });
  return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session) {
  InstanceState* state = nullptr;
  std::unique_ptr<Session> ours;
  try {
    std::unique_lock lock(G().mMutex);
    state = FindInstanceOfSession(session);
    std::erase_if(G().mSessions, [&](const auto& entry) { return entry.first == session; });
    if (G().mAttached && G().mAttached->Handle() == session) {
      ours = std::move(G().mAttached);
    }
  } catch (...) {
    ThreadUtil::ReportEscapedException("xrDestroySession");
  }
  if (!state || !state->mNext.xrDestroySession) {
    return XR_ERROR_HANDLE_INVALID;
  }
  // Our swapchains and spaces go first (outside our lock: runtime calls)
  ThreadUtil::CatchAll("xrDestroySession cleanup", [&] { ours.reset(); });
  return state->mNext.xrDestroySession(session);
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
  InstanceState* state = nullptr;
  XrFrameEndInfo composed {};
  bool useComposed = false;
  try {
    std::shared_lock lock(G().mMutex);
    state = FindInstanceOfSession(session);
    if (state && info && G().mAttached && G().mAttached->Handle() == session) {
      composed = G().mAttached->Compose(*info);
      useComposed = true;
    }
  } catch (...) {
    ThreadUtil::ReportEscapedException("xrEndFrame");
    useComposed = false;
  }
  if (!state || !state->mNext.xrEndFrame) {
    return XR_ERROR_HANDLE_INVALID;
  }
  return state->mNext.xrEndFrame(session, useComposed ? &composed : info);
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrPollEvent(XrInstance instance, XrEventDataBuffer* event) {
  PFN_xrPollEvent next = nullptr;
  {
    std::shared_lock lock(G().mMutex);
    if (const auto* state = FindInstance(instance)) {
      next = state->mNext.xrPollEvent;
    }
  }
  if (!next) {
    return XR_ERROR_HANDLE_INVALID;
  }
  const auto result = next(instance, event);
  if (result == XR_SUCCESS && event) {
    ThreadUtil::CatchAll("xrPollEvent", [&] {
      if (event->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(event);
        Log::Info("Session state: {}", static_cast<int>(changed.state));
      } else if (event->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        const auto& changed = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(event);
        Log::Info("Reference space {} is changing (recenter)", static_cast<int>(changed.referenceSpaceType));
        // LOCAL's new origin may not be at the head (VDXR, test round 13):
        // panels go where the head is once the change is made (ADR-037)
        if (changed.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
          std::shared_lock lock(G().mMutex);
          if (G().mAttached && G().mAttached->Handle() == changed.session) {
            G().mAttached->RecenterAt(changed.changeTime);
          }
        }
      }
    });
  }
  return result;
}

XRAPI_ATTR XrResult XRAPI_CALL
Hook_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
  if (
    !info || !instance || !layerInfo || !layerInfo->nextInfo
    || layerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO
    || !layerInfo->nextInfo->nextGetInstanceProcAddr || !layerInfo->nextInfo->nextCreateApiLayerInstance) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  const auto* nextInfo = layerInfo->nextInfo;
  const auto nextGetProcAddr = nextInfo->nextGetInstanceProcAddr;
  const auto nextCreate = nextInfo->nextCreateApiLayerInstance;
  // The layers below us see the chain from the next entry on
  XrApiLayerCreateInfo chained = *layerInfo;
  chained.nextInfo = nextInfo->next;
  G().mLastNextGetInstanceProcAddr.store(nextGetProcAddr);

  auto state = std::make_unique<InstanceState>();
  ExtensionPlan plan;
  XrInstanceCreateInfo modified = *info;
  bool prepared = false;
  ThreadUtil::CatchAll("xrCreateInstance preparation", [&] {
    StartOnce();
    const std::span<const char* const> gameExtensions {info->enabledExtensionNames, info->enabledExtensionCount};
    state->mApplicationName = info->applicationInfo.applicationName;
    // The diagnostics page and self-test (M6c): what sits between us and the
    // runtime (CMP-1, CMP-2), and whether the game may submit depth (CMP-7)
    state->mChain = SummarizeChain(nextInfo, Product::LayerName);
    state->mDepthEnabled = EnablesExtension(gameExtensions, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    Log::Info("API layers below ours, toward the runtime: {}", state->mChain.Describe());
    if (state->mDepthEnabled) {
      Log::Info("The game enables XR_KHR_composition_layer_depth: with depth submitted, some runtimes hide overlays "
                "behind the game (CMP-7)");
    } else {
      Log::Info("The game doesn't enable XR_KHR_composition_layer_depth");
    }
    state->mConfig = LoadLayerConfig();
    const auto exe = ProcessUtil::ExecutablePath().filename().wstring();
    if (state->mConfig.IsDisabledFor(exe)) {
      Log::Info(
        "The layer is off for {} ({}); passing everything through",
        StringUtil::Narrow(exe),
        state->mConfig.mDisabledByEnvironment ? "environment variable" : "settings");
      state->mPassthrough = true;
      plan = WithoutLayerAdditions(gameExtensions);
    } else {
      const auto support = ProbeInstanceExtension(nextGetProcAddr, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
      plan = PlanExtensions(gameExtensions, support);
      Log::Info(
        "Application '{}'; hand tracking below us: {}; enabled by {}",
        state->mApplicationName,
        SupportName(support),
        plan.Who());
    }
    modified.enabledExtensionNames = plan.mNames.data();
    modified.enabledExtensionCount = static_cast<uint32_t>(plan.mNames.size());
    prepared = true;
  });
  if (!prepared) {
    // Our preparation failed: create the game's instance exactly as asked
    return nextCreate(info, &chained, instance);
  }

  auto result = nextCreate(&modified, &chained, instance);
  if (result == XR_ERROR_EXTENSION_NOT_PRESENT && plan.mLayerAddsHandTracking) {
    // XR-7: "unknown" turned out to be "no"; retry once without it
    Log::Info("The runtime refused XR_EXT_hand_tracking; creating the instance without it");
    plan = WithoutLayerAdditions({info->enabledExtensionNames, info->enabledExtensionCount});
    result = nextCreate(info, &chained, instance);
  }
  if (XR_FAILED(result)) {
    Log::Warning("xrCreateInstance failed below us: {}", XrUtil::ResultName(result));
    return result;
  }

  ThreadUtil::CatchAll("xrCreateInstance bookkeeping", [&] {
    state->mInstance = *instance;
    state->mHandTrackingEnabled = plan.mGameEnabledHandTracking || plan.mLayerAddsHandTracking;
    state->mNext.Load(*instance, nextGetProcAddr, state->mHandTrackingEnabled);
    if (state->mNext.xrGetInstanceProperties) {
      XrInstanceProperties properties {XR_TYPE_INSTANCE_PROPERTIES};
      if (XR_SUCCEEDED(state->mNext.xrGetInstanceProperties(*instance, &properties))) {
        state->mRuntimeName = properties.runtimeName;
        Log::Info(
          "Runtime: {} {}.{}.{}",
          properties.runtimeName,
          XR_VERSION_MAJOR(properties.runtimeVersion),
          XR_VERSION_MINOR(properties.runtimeVersion),
          XR_VERSION_PATCH(properties.runtimeVersion));
      }
    }
    if (!state->mPassthrough && !state->mNext.mHasCore) {
      Log::Error("The next layer lacks functions the layer needs; passing this instance through");
      state->mPassthrough = true;
    }
    std::unique_lock lock(G().mMutex);
    G().mInstances.push_back(std::move(state));
  });
  return result;
}

XRAPI_ATTR XrResult XRAPI_CALL
Hook_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
  if (!name || !function) {
    return XR_ERROR_VALIDATION_FAILURE;
  }
  try {
    const std::string_view fn {name};
    if (fn == "xrGetInstanceProcAddr") {
      *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrGetInstanceProcAddr);
      return XR_SUCCESS;
    }

    if (instance == XR_NULL_HANDLE) {
      // XR-8: let layers above probe through us
      if (const auto next = G().mLastNextGetInstanceProcAddr.load()) {
        return next(instance, name, function);
      }
      *function = nullptr;
      const bool preInstance = fn == "xrEnumerateInstanceExtensionProperties"
        || fn == "xrEnumerateApiLayerProperties" || fn == "xrCreateInstance";
      return preInstance ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_ERROR_HANDLE_INVALID;
    }

    const InstanceState* state = nullptr;
    {
      std::shared_lock lock(G().mMutex);
      state = FindInstance(instance);
    }
    if (!state || !state->mNext.xrGetInstanceProcAddr) {
      *function = nullptr;
      return XR_ERROR_HANDLE_INVALID;
    }
    const auto ours = [&](auto hook) {
      *function = reinterpret_cast<PFN_xrVoidFunction>(hook);
      return XR_SUCCESS;
    };
    if (fn == "xrDestroyInstance") {
      return ours(&Hook_xrDestroyInstance);
    }
    if (!state->mPassthrough) {
      if (fn == "xrCreateSession") {
        return ours(&Hook_xrCreateSession);
      }
      if (fn == "xrDestroySession") {
        return ours(&Hook_xrDestroySession);
      }
      if (fn == "xrEndFrame") {
        return ours(&Hook_xrEndFrame);
      }
      if (fn == "xrPollEvent" && state->mNext.xrPollEvent) {
        return ours(&Hook_xrPollEvent);
      }
    }
    return state->mNext.xrGetInstanceProcAddr(instance, name, function);
  } catch (...) {
    ThreadUtil::ReportEscapedException("xrGetInstanceProcAddr");
    *function = nullptr;
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

}// namespace
}// namespace OpenDesktopVR::Layer

// ---- Exports -----------------------------------------------------------------------

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
  const XrNegotiateLoaderInfo* loaderInfo,
  const char* layerName,
  XrNegotiateApiLayerRequest* request) {
  using namespace OpenDesktopVR;
  try {
    if (
      !loaderInfo || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO
      || loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION
      || loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)
      || request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST
      || request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION
      || request->structSize != sizeof(XrNegotiateApiLayerRequest)) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (layerName && Product::LayerName != layerName) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (
      loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION
      || loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION
      || loaderInfo->minApiVersion > XR_CURRENT_API_VERSION
      || loaderInfo->maxApiVersion < XR_MAKE_VERSION(1, 0, 0)) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = &Layer::Hook_xrGetInstanceProcAddr;
    request->createApiLayerInstance = &Layer::Hook_xrCreateApiLayerInstance;
    return XR_SUCCESS;
  } catch (...) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
  }
  // Nothing on detach: no cleanup at process exit (XR-17)
  return TRUE;
}
