// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The real layer DLL between a minimal fake runtime and a fake game, for the
// M1 cases the simulator doesn't script: negotiation, pre-instance lookups
// (XR-8), zero-layer frames (XR-6), another thread (XR-5), the layer limit
// (XR-9), a failed image wait (XR-15), a second session (XR-18), an
// unsupported graphics API, the disable variable, the game's D3D11 state
// (XR-20), and what the layer logs about the chain and depth (M6c).
//
// The layer's data folder (settings, its log) is a private temporary one.
//
// Usage: layer-dll-test <layer DLL>

#include "check.hpp"
#include "fake-openxr.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

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

using namespace OpenDesktopVR::FakeOpenXR;

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
  // Never the user's own settings or logs
  TempFolder dataFolder;
  _putenv_s("OPENDESKTOPVR_DATA_FOLDER", StringUtil::Narrow(dataFolder.Path()).c_str());

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

  // XR-8 (revised 2026-11-19, Q-1): what a layer above gets when it probes
  // through ours with a null instance before calling down, while no instance
  // of ours is alive
  const auto probeFromAbove = [&](const std::string& when) {
    const int runtimeAsked = gRT.mNullInstanceLookups;
    PFN_xrVoidFunction fn = nullptr;
    const auto layers = layer.mRequest.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", &fn);
    Check(XR_SUCCEEDED(layers) && fn, when + ": the layer properties: answered by the layer");
    if (fn) {
      const auto enumerate = reinterpret_cast<PFN_xrEnumerateApiLayerProperties>(fn);
      uint32_t count = 0;
      XrApiLayerProperties properties {XR_TYPE_API_LAYER_PROPERTIES};
      const bool listed = XR_SUCCEEDED(enumerate(0, &count, nullptr)) && count == 1
        && XR_SUCCEEDED(enumerate(1, &count, &properties)) && Product::LayerName == properties.layerName;
      Check(listed, when + ": they list this layer");
    }
    for (const char* name: {"xrEnumerateInstanceExtensionProperties", "xrCreateInstance", "xrGetSystem"}) {
      fn = reinterpret_cast<PFN_xrVoidFunction>(&F_xrDestroyInstance);// anything but null
      const auto result = layer.mRequest.getInstanceProcAddr(XR_NULL_HANDLE, name, &fn);
      Check(result == XR_ERROR_FUNCTION_UNSUPPORTED && !fn, std::format("{}: {} fails cleanly (unsupported)", when, name));
    }
    Check(gRT.mNullInstanceLookups == runtimeAsked, when + ": nothing was forwarded to the layer below");
  };

  Scenario("XR-8: before the first instance, a layer above gets clean answers", [&] {
    probeFromAbove("before any instance");
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

  Scenario("XR-8: while an instance lives, null-instance lookups reach its next layer", [&] {
    Game game {layer};
    const int runtimeAsked = gRT.mNullInstanceLookups;
    PFN_xrVoidFunction fn = nullptr;
    const auto result = layer.mRequest.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn);
    Check(
      XR_SUCCEEDED(result) && fn == reinterpret_cast<PFN_xrVoidFunction>(&F_xrEnumerateInstanceExtensionProperties)
        && gRT.mNullInstanceLookups == runtimeAsked + 1,
      "reaches the next layer's function");
  });

  Scenario("XR-8: once that instance is destroyed, its next layer is forgotten", [&] {
    probeFromAbove("for a second instance, after the first was destroyed");
    // Two unsupported asks for the same function (one per probe): logged once
    const auto log = std::filesystem::path {dataFolder.Path()} / L"logs" / L"layer-layer-dll-test.log";
    const std::string line = "A layer above asked for xrEnumerateInstanceExtensionProperties with no instance";
    std::string text;
    for (int i = 0; i < 40 && text.find(line) == std::string::npos; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      text = FileUtil::ReadAll(log).value_or("");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));// anything still on its way
    text = FileUtil::ReadAll(log).value_or("");
    int times = 0;
    for (auto at = text.find(line); at != std::string::npos; at = text.find(line, at + 1)) {
      ++times;
    }
    Check(times == 1, std::format("an unsupported lookup is logged once per function (logged {} times)", times));
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

  Scenario("The layer logs the layers below it and the game's depth extension (M6c: CMP-1, CMP-2, CMP-7)", [&] {
    {
      Game plain {layer};
    }
    Game game {layer, {"XR_APILAYER_NOVENDOR_XRNeckSafer", "XR_APILAYER_ULTRALEAP_hand_tracking"},
      {XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME}};
    Check(game.mInstance != XR_NULL_HANDLE, "instance created through a longer chain");
    // The layer's log thread writes within a moment
    const auto log = std::filesystem::path {dataFolder.Path()} / L"logs" / L"layer-layer-dll-test.log";
    std::string text;
    for (int i = 0; i < 40; ++i) {
      text = FileUtil::ReadAll(log).value_or("");
      if (text.find("XR_APILAYER_ULTRALEAP_hand_tracking") != std::string::npos) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Check(text.find("API layers below ours, toward the runtime: none") != std::string::npos, "none below: said so");
    Check(
      text.find("API layers below ours, toward the runtime: XR_APILAYER_NOVENDOR_XRNeckSafer, XR_APILAYER_ULTRALEAP_hand_tracking")
        != std::string::npos,
      "the layers below, nearest first, are logged");
    Check(text.find("The game doesn't enable XR_KHR_composition_layer_depth") != std::string::npos, "no depth: said so");
    Check(text.find("The game enables XR_KHR_composition_layer_depth") != std::string::npos, "depth enabled: logged (CMP-7)");
    Check(gRT.mEnabledExtensions.size() >= 2, "the game's depth extension reached the runtime");
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
