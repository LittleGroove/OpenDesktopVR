// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The real layer DLL between a minimal fake runtime and a fake game, for the
// M1 cases the simulator doesn't script: negotiation, pre-instance lookups
// (XR-8), zero-layer frames (XR-6), another thread (XR-5), the layer limit
// (XR-9), a failed image wait (XR-15), a second session (XR-18), an
// unsupported graphics API, the disable variable, and the game's D3D11 state
// (XR-20).
//
// Usage: layer-dll-test <layer DLL>

#include "check.hpp"

#include <OpenDesktopVR/Product.hpp>

#include <Windows.h>

#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <loader_interfaces.h>

#include <winrt/base.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;

namespace {

// ---- Fake runtime ------------------------------------------------------------

struct FakeRuntime {
  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext> mContext;
  uint64_t mNextHandle {0x100};
  uint32_t mMaxLayers {16};
  bool mFailNextWait {false};
  int mSwapchainsCreated {0};
  int mSwapchainsDestroyed {0};
  int mReleases {0};
  std::map<uint64_t, std::vector<winrt::com_ptr<ID3D11Texture2D>>> mSwapchains;
  // What the last xrEndFrame received
  uint32_t mLastLayerCount {0};
  const XrCompositionLayerBaseHeader* const* mLastLayers {nullptr};
  std::vector<XrStructureType> mLastTypes;
  XrSpace mLastQuadSpace {XR_NULL_HANDLE};
  XrCompositionLayerFlags mLastQuadFlags {0};
  std::vector<std::string> mEnabledExtensions;
};
FakeRuntime gRT;

template <class T>
T NewHandle() {
  return reinterpret_cast<T>(gRT.mNextHandle++);
}

XRAPI_ATTR XrResult XRAPI_CALL F_xrEnumerateInstanceExtensionProperties(
  const char*, uint32_t capacity, uint32_t* count, XrExtensionProperties* properties) {
  *count = 2;
  if (capacity >= 2) {
    strcpy_s(properties[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    strcpy_s(properties[1].extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
F_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo*, XrInstance* instance) {
  gRT.mEnabledExtensions.clear();
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    gRT.mEnabledExtensions.emplace_back(info->enabledExtensionNames[i]);
  }
  *instance = NewHandle<XrInstance>();
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroyInstance(XrInstance) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
  strcpy_s(properties->systemName, "Fake");
  properties->graphicsProperties = {4096, 4096, gRT.mMaxLayers};
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session) {
  *session = NewHandle<XrSession>();
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroySession(XrSession) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) {
  *space = NewHandle<XrSpace>();
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroySpace(XrSpace) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
  location->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  location->pose = {{0, 0, 0, 1}, {0, 1.2f, 0}};
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrEnumerateSwapchainFormats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats) {
  *count = 1;
  if (capacity >= 1) {
    formats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
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
XRAPI_ATTR XrResult XRAPI_CALL F_xrDestroySwapchain(XrSwapchain swapchain) {
  gRT.mSwapchains.erase(reinterpret_cast<uint64_t>(swapchain));
  ++gRT.mSwapchainsDestroyed;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
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
XRAPI_ATTR XrResult XRAPI_CALL F_xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
  *index = 0;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) {
  if (gRT.mFailNextWait) {
    gRT.mFailNextWait = false;
    return XR_ERROR_RUNTIME_FAILURE;
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
  ++gRT.mReleases;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL F_xrEndFrame(XrSession, const XrFrameEndInfo* info) {
  gRT.mLastLayerCount = info->layerCount;
  gRT.mLastLayers = info->layers;
  gRT.mLastTypes.clear();
  for (uint32_t i = 0; i < info->layerCount; ++i) {
    gRT.mLastTypes.push_back(info->layers[i]->type);
    if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
      gRT.mLastQuadSpace = info->layers[i]->space;
      gRT.mLastQuadFlags = info->layers[i]->layerFlags;
    }
  }
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL F_xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
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

int CountType(XrStructureType type) {
  return static_cast<int>(std::count(gRT.mLastTypes.begin(), gRT.mLastTypes.end(), type));
}

}// namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("Usage: layer-dll-test <layer DLL>\n");
    return 2;
  }
  // WARP: runs on CI machines without a GPU
  winrt::check_hresult(D3D11CreateDevice(
    nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
    gRT.mDevice.put(), nullptr, gRT.mContext.put()));
  _putenv_s(std::string {Product::TestPanelEnvVar}.c_str(), "1");

  Layer layer;
  layer.mModule = LoadLibraryA(argv[1]);
  if (!layer.mModule) {
    std::printf("Couldn't load %s\n", argv[1]);
    return 1;
  }

  Scenario("Negotiation accepts only our layer name", [&] {
    Check(XR_FAILED(layer.Negotiate("XR_APILAYER_SOMEONE_else")), "another name is refused");
    Check(XR_SUCCEEDED(layer.Negotiate(std::string {Product::LayerName}.c_str())), "our name is accepted");
    Check(layer.mRequest.getInstanceProcAddr && layer.mRequest.createApiLayerInstance, "entry points returned");
  });

  Scenario("The test panel is composed as a world-locked quad", [&] {
    Game game {layer};
    Check(game.mInstance != XR_NULL_HANDLE, "instance created");
    Check(
      std::ranges::find(gRT.mEnabledExtensions, std::string {XR_EXT_HAND_TRACKING_EXTENSION_NAME}) != gRT.mEnabledExtensions.end(),
      "hand tracking added for the game");
    Check(XR_SUCCEEDED(game.CreateSession()), "session created");
    const int releasesBefore = gRT.mReleases;
    for (int i = 0; i < 50; ++i) {
      game.EndFrame();
    }
    Check(gRT.mLastLayerCount == 2 && gRT.mLastTypes[0] == XR_TYPE_COMPOSITION_LAYER_PROJECTION, "game layer first, then ours");
    Check(CountType(XR_TYPE_COMPOSITION_LAYER_QUAD) == 1, "one quad");
    Check(gRT.mLastQuadFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, "alpha-blended, premultiplied");
    Check(
      gRT.mReleases - releasesBefore == 1,
      std::format("the static image was copied once in 50 frames (got {})", gRT.mReleases - releasesBefore));
  });

  Scenario("XR-8: null-instance lookups are forwarded to the next layer", [&] {
    PFN_xrVoidFunction fn = nullptr;
    const auto result = layer.mRequest.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn);
    Check(
      XR_SUCCEEDED(result) && fn == reinterpret_cast<PFN_xrVoidFunction>(&F_xrEnumerateInstanceExtensionProperties),
      "reaches the next layer's function");
  });

  Scenario("XR-6, XR-5, XR-9: zero layers, another thread, the layer limit", [&] {
    Game game {layer};
    game.CreateSession();
    game.EndFrame();
    game.EndFrame(0);
    Check(gRT.mLastLayerCount == 0 && gRT.mLastLayers == nullptr, "a zero-layer frame passes through untouched");
    std::thread other([&] { game.EndFrame(); });
    other.join();
    Check(gRT.mLastLayerCount == 2, "xrEndFrame from another thread composes the same");
    game.EndFrame(16);
    Check(gRT.mLastLayerCount == 16 && CountType(XR_TYPE_COMPOSITION_LAYER_QUAD) == 0, "the game at the limit: nothing added");
    game.EndFrame(15);
    Check(gRT.mLastLayerCount == 16 && CountType(XR_TYPE_COMPOSITION_LAYER_QUAD) == 1, "one slot free: the panel fits");
  });

  Scenario("XR-15: a failed image wait re-creates the swapchain, then drawing resumes", [&] {
    Game game {layer};
    gRT.mFailNextWait = true;
    const int created = gRT.mSwapchainsCreated;
    const int destroyed = gRT.mSwapchainsDestroyed;
    game.CreateSession();
    game.EndFrame();
    Check(CountType(XR_TYPE_COMPOSITION_LAYER_QUAD) == 0, "no image yet: the panel isn't submitted");
    game.EndFrame();
    Check(gRT.mSwapchainsCreated - created == 2 && gRT.mSwapchainsDestroyed - destroyed == 1, "destroyed and re-created");
    Check(CountType(XR_TYPE_COMPOSITION_LAYER_QUAD) == 1, "the panel shows after the re-create");
  });

  Scenario("XR-18: a second concurrent session passes through", [&] {
    Game first {layer};
    first.CreateSession();
    Game second {layer};
    second.CreateSession();
    second.EndFrame();
    Check(gRT.mLastLayerCount == 1, "the second session's frame is unchanged");
    first.EndFrame();
    Check(gRT.mLastLayerCount == 2, "the first still gets panels");
  });

  Scenario("An unsupported graphics API passes through without failing", [&] {
    Game game {layer};
    Check(XR_SUCCEEDED(game.CreateSession(XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)), "D3D12 session creation still succeeds");
    game.EndFrame();
    Check(gRT.mLastLayerCount == 1, "frames pass through unchanged");
  });

  Scenario("XR-20: the game's Direct3D 11 state is untouched", [&] {
    Game game {layer};
    game.CreateSession();
    const D3D11_VIEWPORT viewport {12, 34, 560, 780, 0, 1};
    gRT.mContext->RSSetViewports(1, &viewport);
    gRT.mFailNextWait = false;
    game.EndFrame();// copies the test card (first frame of a new swapchain)
    UINT count = 1;
    D3D11_VIEWPORT after {};
    gRT.mContext->RSGetViewports(&count, &after);
    Check(count == 1 && std::memcmp(&after, &viewport, sizeof(viewport)) == 0, "the viewport is unchanged");
  });

  Scenario("The disable variable turns the layer into a pass-through", [&] {
    _putenv_s(std::string {Product::LayerDisableEnvVar}.c_str(), "1");
    Game game {layer};
    const auto endFrame = layer.Get<PFN_xrEndFrame>(game.mInstance, "xrEndFrame");
    Check(endFrame == &F_xrEndFrame, "xrEndFrame goes straight to the next layer");
    Check(gRT.mEnabledExtensions.size() == 1, "no extensions added");
    _putenv_s(std::string {Product::LayerDisableEnvVar}.c_str(), "");
  });

  return Finish();
}
