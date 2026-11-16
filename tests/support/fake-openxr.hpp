// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// A minimal fake OpenXR runtime and fake game for loading the real layer DLL
// in tests (layer-dll-test, host-layer-test). Our own test code, written
// from the OpenXR specification and loader documentation.
//
//   Layer layer; layer.mModule = LoadLibraryA(dll); layer.Negotiate(name);
//   Game game {layer}; game.CreateSession(); game.EndFrame();
//   gRT.mLastLayerCount, gRT.mLastQuads, gRT.mReleasesBySwapchain ...
#pragma once

#include <OpenDesktopVR/Product.hpp>

#include <Windows.h>

#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <loader_interfaces.h>

#include <winrt/base.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace OpenDesktopVR::FakeOpenXR {

// ---- Fake runtime ------------------------------------------------------------

struct FakeRuntime {
  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext> mContext;
  uint64_t mNextHandle {0x100};
  uint32_t mMaxLayers {16};
  bool mFailNextWait {false};
  // Where xrLocateSpace says the head is (tests turn it for gaze focus)
  XrPosef mHeadPose {{0, 0, 0, 1}, {0, 1.2f, 0}};
  int mSwapchainsCreated {0};
  int mSwapchainsDestroyed {0};
  int mReleases {0};
  std::map<uint64_t, int> mReleasesBySwapchain;
  std::map<uint64_t, std::vector<winrt::com_ptr<ID3D11Texture2D>>> mSwapchains;
  // What the last xrEndFrame received
  uint32_t mLastLayerCount {0};
  const XrCompositionLayerBaseHeader* const* mLastLayers {nullptr};
  std::vector<XrStructureType> mLastTypes;
  XrSpace mLastQuadSpace {XR_NULL_HANDLE};
  std::vector<XrCompositionLayerQuad> mLastQuads;
  XrCompositionLayerFlags mLastQuadFlags {0};
  std::vector<std::string> mEnabledExtensions;
};
inline FakeRuntime gRT;

template <class T>
inline T NewHandle() {
  return reinterpret_cast<T>(gRT.mNextHandle++);
}

inline XRAPI_ATTR XrResult XRAPI_CALL F_xrEnumerateInstanceExtensionProperties(
  const char*, uint32_t capacity, uint32_t* count, XrExtensionProperties* properties) {
  *count = 2;
  if (capacity >= 2) {
    strcpy_s(properties[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    strcpy_s(properties[1].extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
  }
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL
F_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo*, XrInstance* instance) {
  gRT.mEnabledExtensions.clear();
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    gRT.mEnabledExtensions.emplace_back(info->enabledExtensionNames[i]);
  }
  *instance = NewHandle<XrInstance>();
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroyInstance(XrInstance) {
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
  strcpy_s(properties->systemName, "Fake");
  properties->graphicsProperties = {4096, 4096, gRT.mMaxLayers};
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session) {
  *session = NewHandle<XrSession>();
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroySession(XrSession) {
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) {
  *space = NewHandle<XrSpace>();
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroySpace(XrSpace) {
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
  location->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  location->pose = gRT.mHeadPose;
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrEnumerateSwapchainFormats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats) {
  *count = 1;
  if (capacity >= 1) {
    formats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  }
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
  const D3D11_TEXTURE2D_DESC desc {
    info->width, info->height, 1, 1, DXGI_FORMAT_B8G8R8A8_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
  std::vector<winrt::com_ptr<ID3D11Texture2D>> images(3);
  for (auto& image: images) {
    if (FAILED(gRT.mDevice->CreateTexture2D(&desc, nullptr, image.put()))) {
      return XR_ERROR_RUNTIME_FAILURE;
    }
  }
  *swapchain = NewHandle<XrSwapchain>();
  gRT.mSwapchains[reinterpret_cast<uint64_t>(*swapchain)] = std::move(images);
  ++gRT.mSwapchainsCreated;
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroySwapchain(XrSwapchain swapchain) {
  gRT.mSwapchains.erase(reinterpret_cast<uint64_t>(swapchain));
  ++gRT.mSwapchainsDestroyed;
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL
F_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
  const auto& textures = gRT.mSwapchains.at(reinterpret_cast<uint64_t>(swapchain));
  *count = static_cast<uint32_t>(textures.size());
  if (capacity >= textures.size()) {
    auto* d3d = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (std::size_t i = 0; i < textures.size(); ++i) {
      d3d[i].texture = textures[i].get();
    }
  }
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
  *index = 0;
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) {
  if (gRT.mFailNextWait) {
    gRT.mFailNextWait = false;
    return XR_ERROR_RUNTIME_FAILURE;
  }
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) {
  ++gRT.mReleases;
  ++gRT.mReleasesBySwapchain[reinterpret_cast<uint64_t>(swapchain)];
  return XR_SUCCESS;
}
inline XRAPI_ATTR XrResult XRAPI_CALL F_xrEndFrame(XrSession, const XrFrameEndInfo* info) {
  gRT.mLastLayerCount = info->layerCount;
  gRT.mLastLayers = info->layers;
  gRT.mLastTypes.clear();
  gRT.mLastQuads.clear();
  for (uint32_t i = 0; i < info->layerCount; ++i) {
    gRT.mLastTypes.push_back(info->layers[i]->type);
    if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
      gRT.mLastQuads.push_back(*reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]));
      gRT.mLastQuadSpace = info->layers[i]->space;
      gRT.mLastQuadFlags = info->layers[i]->layerFlags;
    }
  }
  return XR_SUCCESS;
}

inline XRAPI_ATTR XrResult XRAPI_CALL F_xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
#define F(fn) {#fn, reinterpret_cast<PFN_xrVoidFunction>(&F_##fn)}
  static const std::map<std::string, PFN_xrVoidFunction> functions {
    F(xrGetInstanceProcAddr), F(xrEnumerateInstanceExtensionProperties), F(xrDestroyInstance), F(xrGetSystemProperties),
    F(xrCreateSession), F(xrDestroySession), F(xrCreateReferenceSpace), F(xrDestroySpace), F(xrLocateSpace),
    F(xrEnumerateSwapchainFormats), F(xrCreateSwapchain), F(xrDestroySwapchain), F(xrEnumerateSwapchainImages),
    F(xrAcquireSwapchainImage), F(xrWaitSwapchainImage), F(xrReleaseSwapchainImage), F(xrEndFrame),
  };
#undef F
  const auto it = functions.find(name);
  *function = it == functions.end() ? nullptr : it->second;
  return *function ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---- Fake game ---------------------------------------------------------------

struct Layer {
  HMODULE mModule {nullptr};
  XrNegotiateApiLayerRequest mRequest {
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(XrNegotiateApiLayerRequest)};

  XrResult Negotiate(const char* name) {
    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
      GetProcAddress(mModule, "xrNegotiateLoaderApiLayerInterface"));
    XrNegotiateLoaderInfo info {
      XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo),
      1, XR_CURRENT_LOADER_API_LAYER_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff)};
    return negotiate ? negotiate(&info, name, &mRequest) : XR_ERROR_INITIALIZATION_FAILED;
  }

  template <class PFN>
  PFN Get(XrInstance instance, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    mRequest.getInstanceProcAddr(instance, name, &fn);
    return reinterpret_cast<PFN>(fn);
  }
};

struct Game {
  Layer& mLayer;
  XrInstance mInstance {XR_NULL_HANDLE};
  XrSession mSession {XR_NULL_HANDLE};
  std::vector<XrCompositionLayerProjection> mLayers;
  std::vector<const XrCompositionLayerBaseHeader*> mHeaders;
  XrTime mTime {1};

  explicit Game(Layer& layer) : mLayer(layer) {
    XrApiLayerNextInfo next {
      XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
    strcpy_s(next.layerName, std::string {Product::LayerName}.c_str());
    next.nextGetInstanceProcAddr = &F_xrGetInstanceProcAddr;
    next.nextCreateApiLayerInstance = &F_xrCreateApiLayerInstance;
    XrApiLayerCreateInfo layerInfo {
      XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO, XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(XrApiLayerCreateInfo)};
    layerInfo.nextInfo = &next;
    const char* extensions[] {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info {XR_TYPE_INSTANCE_CREATE_INFO};
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    strcpy_s(info.applicationInfo.applicationName, "layer-dll-test");
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    mLayer.mRequest.createApiLayerInstance(&info, &layerInfo, &mInstance);
  }

  XrResult CreateSession(XrStructureType binding = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
    XrGraphicsBindingD3D11KHR d3d11 {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, nullptr, gRT.mDevice.get()};
    XrBaseInStructure other {binding, nullptr};
    XrSessionCreateInfo info {XR_TYPE_SESSION_CREATE_INFO};
    info.next = binding == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR ? static_cast<const void*>(&d3d11) : &other;
    info.systemId = 1;
    return mLayer.Get<PFN_xrCreateSession>(mInstance, "xrCreateSession")(mInstance, &info, &mSession);
  }

  // Ends a frame with `count` projection layers
  XrResult EndFrame(uint32_t count = 1) {
    mLayers.assign(count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION});
    mHeaders.clear();
    for (const auto& layer: mLayers) {
      mHeaders.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
    }
    XrFrameEndInfo info {XR_TYPE_FRAME_END_INFO};
    info.displayTime = mTime++;
    info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    info.layerCount = count;
    info.layers = count ? mHeaders.data() : nullptr;
    return mLayer.Get<PFN_xrEndFrame>(mInstance, "xrEndFrame")(mSession, &info);
  }

  ~Game() {
    if (mSession) {
      mLayer.Get<PFN_xrDestroySession>(mInstance, "xrDestroySession")(mSession);
    }
    if (mInstance) {
      mLayer.Get<PFN_xrDestroyInstance>(mInstance, "xrDestroyInstance")(mInstance);
    }
  }
};

inline int CountType(XrStructureType type) {
  return static_cast<int>(std::count(gRT.mLastTypes.begin(), gRT.mLastTypes.end(), type));
}

}// namespace OpenDesktopVR::FakeOpenXR
