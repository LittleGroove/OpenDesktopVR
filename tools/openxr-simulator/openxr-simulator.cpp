// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// End-to-end test of hand tracking without a headset.
//
// This program is both a fake OpenXR game and a fake OpenXR runtime with
// scripted hands. It loads the real OpenXR API layer between them, exactly as
// the OpenXR loader would. With the host app running, the scripted hand
// points at the real panel and taps it; the layer turns that into finger
// events for the host app, which clicks whatever the panel shows.
//
// Usage:
//   openxr-simulator [--negotiate-export <name>] [--layer-name <name>]
//     [--early-lookups-fail] [--no-hand-tracking]
//     <layer DLL> <output folder>
//     [click log | --watch <s> | --type-keys ... | --type <text> <log> |
//      --bench <s> [hover]]
//
// --negotiate-export: the layer DLL's loader negotiation export (default
//   xrNegotiateLoaderApiLayerInterface).
// --layer-name: the layer name passed to it (default XR_APILAYER_TEST_layer).
// --early-lookups-fail: behave like an API layer below ours that can't
//   resolve functions until its instance exists (some capture/mirror layers
//   answer "unsupported" before then).
// --no-hand-tracking: a runtime without XR_EXT_hand_tracking; only instance
//   creation is checked.
// --stage-offset: the game submits in a STAGE space that is offset (moved
//   and turned) from LOCAL, as some games and shims do; the drawn hand must
//   still be where the hand is (XR-12). Added in the port (M4a).
// --bench <s> [hover]: measure the layer's GPU time per frame and what it
//   hands the runtime to composite, optionally with a pointing hand hovering
//   in front of the panel. No read-backs.
// --gestures: with a click log, the rest of doc 11's hand scenarios instead
//   of the tap script: an angled poke, hold 1 s then drag 40 mm, two-finger
//   scroll 36 mm, tracking gaps of 300 and 100 ms during drags, reaching
//   behind the panel, and the hand layer's image wait failing once. Added in
//   M4c.
// --depth-order: the host shows overlapping panels at different depths;
//   every frame's panel layers must be back to front (doc 11). Added in M4c.
// --recenter: test round 13's finding (ADR-037). With the host's test
//   panels: the head moves, then the runtime recenters as VDXR did, moving
//   LOCAL's origin to 45 cm above the head (the event for LOCAL, then the
//   change); panels must then sit from the head as they sat from LOCAL's
//   origin at the start. Then the host's "Recenter" keybind (its
//   --test-recenter-after) must do the same at the head's new place.
// --quick-menu: the quick menu, tapped (M6d, ADR-041). With the host's
//   --show-quick-menu and its two test panels: the open menu is found by
//   its width and must be laid out as QuickMenu::BuildLayout says; tapping
//   the first panel's switch hides that panel's layer and tapping it again
//   brings it back; the close button leaves the 40 mm button, and tapping
//   that opens the menu again.
// --type <text> <log>: doc 11's typing scenario (M5b). With the keyboard
//   showing (the host's --show-keyboard) and a window panel of
//   edit-target-app writing its text to <log>: tap the panel (where keys
//   go), then poke each letter of <text> (US letters) on the keyboard, at
//   the keys' places from the keyboard's own geometry; the log must end up
//   holding <text>.
//
// The click log is from click-target-test, shown in a panel with input
// enabled; if given, the clicks are checked too.

#include "save-png.hpp"
#include "synthetic-hands.hpp"

#include <OpenDesktopVR/KeyboardGeometry.hpp>
#include <OpenDesktopVR/KeyboardLayout.hpp>
#include <OpenDesktopVR/QuickMenu.hpp>

#include <winrt/base.h>

#include <Windows.h>

#include <d3d11.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <loader_interfaces.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <deque>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::SyntheticHands;

namespace {

template <class T>
T ToHandle(uint64_t value) {
  return reinterpret_cast<T>(value);
}

template <class T>
uint64_t FromHandle(T handle) {
  return reinterpret_cast<uint64_t>(handle);
}

struct RigidPose {
  Vector3 mPosition {};
  Quaternion mOrientation {Quaternion::Identity};
};

// The simulated headset: 1.2m up, looking down at the panel (about 50
// degrees), as you would to tap it
RigidPose MakeHMDPose() {
  constexpr float pitch = 0.87f;
  auto orientation = Quaternion::CreateFromAxisAngle(Vector3::UnitX, -pitch);
  if (Vector3::Transform(-Vector3::UnitZ, orientation).y > 0) {
    orientation = Quaternion::CreateFromAxisAngle(Vector3::UnitX, pitch);
  }
  return {{0, 1.2f, 0}, orientation};
}
// Moved by '--recenter' (M5c); fixed otherwise
RigidPose HMDPose = MakeHMDPose();
constexpr float IPD = 0.064f;

XrPosef ToXr(const RigidPose& pose) {
  const auto& p = pose.mPosition;
  const auto& o = pose.mOrientation;
  return {{o.x, o.y, o.z, o.w}, {p.x, p.y, p.z}};
}

struct FakeSwapchain {
  XrSwapchainCreateInfo mInfo {};
  DXGI_FORMAT mTextureFormat {};
  std::vector<winrt::com_ptr<ID3D11Texture2D>> mImages;
  uint32_t mNextIndex {0};
  uint32_t mAcquired {0};
  uint32_t mReleased {0};
};

struct Runtime {
  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext> mContext;
  uint64_t mNextHandle {0x1000};

  std::vector<std::string> mEnabledExtensions;
  // '--early-lookups-fail': like an API layer below ours that can't resolve
  // functions until its instance exists
  bool mEarlyLookupsFail {false};
  // '--no-hand-tracking': a runtime without XR_EXT_hand_tracking
  bool mNoHandTracking {false};
  // '--stage-offset': where STAGE's origin is in the world (LOCAL)
  bool mStageOffset {false};
  RigidPose mStage {};
  bool mInstanceCreated {false};
  std::map<uint64_t, XrReferenceSpaceType> mSpaces;
  // Each space's offset from its type's origin (poseInReferenceSpace)
  std::map<uint64_t, RigidPose> mSpaceOffsets;
  // LOCAL's origin in the world; moved by a recenter ('--recenter')
  RigidPose mLocal {};
  // Events for xrPollEvent
  std::deque<XrEventDataBuffer> mEvents;
  // This frame's panel layers
  std::vector<XrCompositionLayerQuad> mQuads;
  std::map<uint64_t, FakeSwapchain> mSwapchains;
  std::map<uint64_t, XrHandEXT> mHandTrackers;
  int mHandTrackersCreated {0};

  // In world (LOCAL) space; [0] = left, [1] = right
  std::array<HandJoints, 2> mHands {};

  // What the layer submitted in xrEndFrame()
  std::optional<XrCompositionLayerQuad> mQuad;
  bool mHandLayerThisFrame {false};
  int mHandLayerCoverage {0};
  std::optional<std::filesystem::path> mSaveHandLayerTo;
  // Frames where the hand layer's views didn't use the game's own pose and
  // field of view, over their whole image
  int mHandLayerViewMismatches {0};
  // While set, each hand layer is checked: the right index fingertip must
  // fall inside the drawn hand in the left eye's image
  bool mCheckHandPlacement {false};
  int mHandPlacementChecks {0};
  int mHandPlacementMisses {0};

  // Panel layers in the last frame
  int mQuadCount {0};
  // The virtual keyboard's layer, if
  // shown: much wider than tall
  std::optional<XrCompositionLayerQuad> mKeyboardQuad;
  // The last panel layer seen that isn't the keyboard (M5b)
  std::optional<XrCompositionLayerQuad> mPanelQuad;
  // Panel layers must be drawn back
  // to front (a nearer one after a further one); frames where they weren't
  int mFramesWithMultipleQuads {0};
  int mFramesOutOfOrder {0};

  // Watching the panel for the click target's orange click marker
  bool mWatchPanel {false};
  float mWatchU {};
  float mWatchV {};
  std::optional<double> mPanelChangedAt;
  // When the scripted fingertip first came within the touch distance (1 cm)
  // of the panel: the frame whose sample reports contact (added in the
  // port, M4b)
  std::optional<double> mContactAt;

  // '--bench': no read-backs, and counts of what a real runtime would have
  // to composite
  bool mBench {false};
  // Swapchain images the layer released (each is a new image a runtime may
  // copy or re-sample), and their pixels
  uint64_t mReleases {0};
  uint64_t mReleasedPixels {0};
  // Frames with a hand layer, and the sum over those frames of the share of
  // each eye's field of view it covers (what the compositor has to blend)
  int mBenchHandFrames {0};
  double mBenchHandCoverage {0};

  // M4c (doc 11, "xrWaitSwapchainImage fails once"): the hand layer's
  // swapchain, the next wait on it fails once when set, and what followed
  uint64_t mHandSwapchain {0};
  bool mFailNextHandWait {false};
  int mHandWaitFailures {0};
  int mHandSwapchainsDestroyed {0};
  int mHandLayerFramesAfterFailure {0};
  bool mNewHandSwapchainAfterFailure {false};
  uint64_t mFailedHandSwapchain {0};

  // M4c ("Reach behind a panel"): while set, each hand layer is checked:
  // the drawn hand must be where the hand is, not moved to the panel's
  // surface (touch limiting). Distances in the left eye's image, in pixels,
  // from the drawn hand's middle to the real hand's, and to where touch
  // limiting would have drawn it.
  bool mCheckDrawnWhereItIs {false};
  int mDrawnChecks {0};
  float mDrawnWorstOffset {0};
  float mDrawnNearestLimited {1e9f};
};
Runtime gRuntime;

double NowMilliseconds() {
  static const auto frequency = [] {
    LARGE_INTEGER f {};
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER now {};
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) * 1000.0 / frequency;
}

uint64_t NewHandle() {
  return gRuntime.mNextHandle++;
}

// `offset` in a space whose origin is `base`, in the world
RigidPose ComposePose(const RigidPose& base, const RigidPose& offset) {
  Quaternion orientation;
  DirectX::XMStoreFloat4(
    &orientation,
    DirectX::XMQuaternionMultiply(
      DirectX::XMLoadFloat4(&offset.mOrientation),
      DirectX::XMLoadFloat4(&base.mOrientation)));
  return {base.mPosition + Vector3::Transform(offset.mPosition, base.mOrientation), orientation};
}

RigidPose SpacePose(XrSpace space) {
  const auto handle = FromHandle(space);
  const auto it = gRuntime.mSpaces.find(handle);
  // LOCAL's origin is the world's until a recenter; STAGE's too, without
  // --stage-offset
  RigidPose base = gRuntime.mLocal;
  if (it != gRuntime.mSpaces.end() && it->second == XR_REFERENCE_SPACE_TYPE_VIEW) {
    base = HMDPose;
  } else if (it != gRuntime.mSpaces.end() && it->second == XR_REFERENCE_SPACE_TYPE_STAGE) {
    base = gRuntime.mStageOffset ? gRuntime.mStage : RigidPose {};
  }
  if (const auto offset = gRuntime.mSpaceOffsets.find(handle); offset != gRuntime.mSpaceOffsets.end()) {
    return ComposePose(base, offset->second);
  }
  return base;
}

// `pose` (in world space), relative to `base`
RigidPose Relative(const RigidPose& pose, const RigidPose& base) {
  Quaternion inverse;
  base.mOrientation.Inverse(inverse);
  Quaternion orientation;
  DirectX::XMStoreFloat4(
    &orientation,
    DirectX::XMQuaternionMultiply(
      DirectX::XMLoadFloat4(&pose.mOrientation),
      DirectX::XMLoadFloat4(&inverse)));
  return {
    Vector3::Transform(pose.mPosition - base.mPosition, inverse),
    orientation,
  };
}

// A quad's pose in the world (LOCAL), from the space the layer submitted it
// in: the game's, which is STAGE with '--stage-offset'. Added in M4c: the
// script placed hands at the quad's own coordinates, so in STAGE they
// weren't on the panel.
RigidPose QuadWorldPose(const XrCompositionLayerQuad& quad) {
  const auto base = SpacePose(quad.space);
  const auto& p = quad.pose.position;
  const auto& o = quad.pose.orientation;
  const Quaternion local {o.x, o.y, o.z, o.w};
  Quaternion orientation;
  DirectX::XMStoreFloat4(
    &orientation,
    DirectX::XMQuaternionMultiply(
      DirectX::XMLoadFloat4(&local),
      DirectX::XMLoadFloat4(&base.mOrientation)));
  return {
    base.mPosition + Vector3::Transform(Vector3 {p.x, p.y, p.z}, base.mOrientation),
    orientation,
  };
}

HandInteraction::Panel WorldPanel(const XrCompositionLayerQuad& quad) {
  const auto world = QuadWorldPose(quad);
  return {
    .mPosition = world.mPosition,
    .mOrientation = world.mOrientation,
    .mSize = {quad.size.width, quad.size.height},
  };
}

// Where a point in the world falls in the left eye's image, in pixels;
// nothing if it's behind the eye
std::optional<std::pair<float, float>> LeftEyePixel(
  const Vector3& world,
  const XrFovf& fov,
  uint32_t width,
  uint32_t height) {
  RigidPose eye = HMDPose;
  eye.mPosition += Vector3::Transform(Vector3 {-IPD / 2, 0, 0}, HMDPose.mOrientation);
  const auto local = Relative({world, Quaternion::Identity}, eye).mPosition;
  const float forward = -local.z;
  if (forward <= 0) {
    return std::nullopt;
  }
  const float tanX = local.x / forward;
  const float tanY = local.y / forward;
  return std::pair {
    (tanX - std::tan(fov.angleLeft))
      / (std::tan(fov.angleRight) - std::tan(fov.angleLeft)) * width,
    (std::tan(fov.angleUp) - tanY)
      / (std::tan(fov.angleUp) - std::tan(fov.angleDown)) * height,
  };
}

// The middle of the box around a hand's joints in the left eye's image
std::optional<std::pair<float, float>> HandMiddle(
  const HandJoints& hand,
  const XrFovf& fov,
  uint32_t width,
  uint32_t height) {
  float left = 1e9f;
  float top = 1e9f;
  float right = -1e9f;
  float bottom = -1e9f;
  for (const auto& joint: hand.mJoints) {
    const auto pixel = LeftEyePixel(joint.mPosition, fov, width, height);
    if (!pixel) {
      return std::nullopt;
    }
    left = std::min(left, pixel->first);
    right = std::max(right, pixel->first);
    top = std::min(top, pixel->second);
    bottom = std::max(bottom, pixel->second);
  }
  return std::pair {(left + right) / 2, (top + bottom) / 2};
}

///// Fake runtime /////

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrEnumerateInstanceExtensionProperties(
  const char* layerName,
  uint32_t capacity,
  uint32_t* count,
  XrExtensionProperties* properties) {
  if (layerName) {
    return XR_ERROR_API_LAYER_NOT_PRESENT;
  }
  const std::array<std::pair<const char*, uint32_t>, 2> extensions {{
    {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, 9},
    {XR_EXT_HAND_TRACKING_EXTENSION_NAME, 4},
  }};
  const auto listed = gRuntime.mNoHandTracking ? 1 : extensions.size();
  *count = static_cast<uint32_t>(listed);
  if (capacity == 0) {
    return XR_SUCCESS;
  }
  if (capacity < listed) {
    return XR_ERROR_SIZE_INSUFFICIENT;
  }
  for (std::size_t i = 0; i < listed; ++i) {
    strcpy_s(properties[i].extensionName, extensions[i].first);
    properties[i].extensionVersion = extensions[i].second;
  }
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrCreateApiLayerInstance(
  const XrInstanceCreateInfo* info,
  const XrApiLayerCreateInfo*,
  XrInstance* instance) {
  std::vector<std::string> enabled;
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    const std::string_view name {info->enabledExtensionNames[i]};
    if (
      name != XR_KHR_D3D11_ENABLE_EXTENSION_NAME
      && (name != XR_EXT_HAND_TRACKING_EXTENSION_NAME
          || gRuntime.mNoHandTracking)) {
      return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
    enabled.emplace_back(name);
  }
  gRuntime.mEnabledExtensions = std::move(enabled);
  gRuntime.mInstanceCreated = true;
  *instance = ToHandle<XrInstance>(NewHandle());
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
Fake_xrGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
  properties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
  strcpy_s(properties->runtimeName, "Hand tracking simulator");
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrGetSystemProperties(
  XrInstance,
  XrSystemId system,
  XrSystemProperties* properties) {
  properties->systemId = system;
  properties->vendorId = 0;
  strcpy_s(properties->systemName, "Simulated headset");
  properties->graphicsProperties = {4096, 4096, 16};
  properties->trackingProperties = {XR_TRUE, XR_TRUE};
  for (auto next = static_cast<XrBaseOutStructure*>(properties->next); next;
       next = next->next) {
    if (next->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT) {
      reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(next)
        ->supportsHandTracking
        = XR_TRUE;
    }
  }
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
Fake_xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session) {
  *session = ToHandle<XrSession>(NewHandle());
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrDestroySession(XrSession) {
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrDestroyInstance(XrInstance) {
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrCreateReferenceSpace(
  XrSession,
  const XrReferenceSpaceCreateInfo* info,
  XrSpace* space) {
  const auto handle = NewHandle();
  gRuntime.mSpaces[handle] = info->referenceSpaceType;
  const auto& p = info->poseInReferenceSpace;
  gRuntime.mSpaceOffsets[handle] = {
    {p.position.x, p.position.y, p.position.z},
    {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
  };
  *space = ToHandle<XrSpace>(handle);
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrDestroySpace(XrSpace space) {
  gRuntime.mSpaces.erase(FromHandle(space));
  gRuntime.mSpaceOffsets.erase(FromHandle(space));
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrPollEvent(XrInstance, XrEventDataBuffer* event) {
  if (gRuntime.mEvents.empty()) {
    return XR_EVENT_UNAVAILABLE;
  }
  *event = gRuntime.mEvents.front();
  gRuntime.mEvents.pop_front();
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrLocateSpace(
  XrSpace space,
  XrSpace base,
  XrTime,
  XrSpaceLocation* location) {
  location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
    | XR_SPACE_LOCATION_POSITION_VALID_BIT
    | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT
    | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
  location->pose = ToXr(Relative(SpacePose(space), SpacePose(base)));
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrEnumerateSwapchainFormats(
  XrSession,
  uint32_t capacity,
  uint32_t* count,
  int64_t* formats) {
  const std::array<int64_t, 2> supported {
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
  };
  *count = static_cast<uint32_t>(supported.size());
  if (capacity == 0) {
    return XR_SUCCESS;
  }
  if (capacity < supported.size()) {
    return XR_ERROR_SIZE_INSUFFICIENT;
  }
  std::ranges::copy(supported, formats);
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrCreateSwapchain(
  XrSession,
  const XrSwapchainCreateInfo* info,
  XrSwapchain* swapchain) {
  // Like real runtimes, use typeless textures so that apps can pick the view
  // format
  DXGI_FORMAT format = static_cast<DXGI_FORMAT>(info->format);
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
      break;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
      break;
    default:
      return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
  }

  FakeSwapchain fake {.mInfo = *info, .mTextureFormat = format};
  const D3D11_TEXTURE2D_DESC desc {
    .Width = info->width,
    .Height = info->height,
    .MipLevels = 1,
    .ArraySize = info->arraySize,
    .Format = format,
    .SampleDesc = {1, 0},
    .Usage = D3D11_USAGE_DEFAULT,
    .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
  };
  for (int i = 0; i < 3; ++i) {
    winrt::com_ptr<ID3D11Texture2D> texture;
    if (FAILED(gRuntime.mDevice->CreateTexture2D(&desc, nullptr, texture.put()))) {
      return XR_ERROR_RUNTIME_FAILURE;
    }
    fake.mImages.push_back(std::move(texture));
  }
  const auto handle = NewHandle();
  gRuntime.mSwapchains[handle] = std::move(fake);
  *swapchain = ToHandle<XrSwapchain>(handle);
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrDestroySwapchain(XrSwapchain swapchain) {
  if (FromHandle(swapchain) == gRuntime.mHandSwapchain) {
    ++gRuntime.mHandSwapchainsDestroyed;
  }
  gRuntime.mSwapchains.erase(FromHandle(swapchain));
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrEnumerateSwapchainImages(
  XrSwapchain swapchain,
  uint32_t capacity,
  uint32_t* count,
  XrSwapchainImageBaseHeader* images) {
  const auto it = gRuntime.mSwapchains.find(FromHandle(swapchain));
  if (it == gRuntime.mSwapchains.end()) {
    return XR_ERROR_HANDLE_INVALID;
  }
  const auto& fake = it->second;
  *count = static_cast<uint32_t>(fake.mImages.size());
  if (capacity == 0) {
    return XR_SUCCESS;
  }
  if (capacity < fake.mImages.size()) {
    return XR_ERROR_SIZE_INSUFFICIENT;
  }
  auto d3d = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
  for (std::size_t i = 0; i < fake.mImages.size(); ++i) {
    d3d[i].texture = fake.mImages[i].get();
  }
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrAcquireSwapchainImage(
  XrSwapchain swapchain,
  const XrSwapchainImageAcquireInfo*,
  uint32_t* index) {
  auto& fake = gRuntime.mSwapchains.at(FromHandle(swapchain));
  fake.mAcquired = fake.mNextIndex;
  fake.mNextIndex = (fake.mNextIndex + 1) % fake.mImages.size();
  *index = fake.mAcquired;
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
Fake_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo*) {
  if (gRuntime.mFailNextHandWait && gRuntime.mHandSwapchain != 0
      && FromHandle(swapchain) == gRuntime.mHandSwapchain) {
    gRuntime.mFailNextHandWait = false;
    ++gRuntime.mHandWaitFailures;
    gRuntime.mFailedHandSwapchain = gRuntime.mHandSwapchain;
    return XR_ERROR_RUNTIME_FAILURE;
  }
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrReleaseSwapchainImage(
  XrSwapchain swapchain,
  const XrSwapchainImageReleaseInfo*) {
  auto& fake = gRuntime.mSwapchains.at(FromHandle(swapchain));
  fake.mReleased = fake.mAcquired;
  ++gRuntime.mReleases;
  gRuntime.mReleasedPixels
    += static_cast<uint64_t>(fake.mInfo.width) * fake.mInfo.height;
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrCreateHandTrackerEXT(
  XrSession,
  const XrHandTrackerCreateInfoEXT* info,
  XrHandTrackerEXT* tracker) {
  const auto handle = NewHandle();
  gRuntime.mHandTrackers[handle] = info->hand;
  ++gRuntime.mHandTrackersCreated;
  *tracker = ToHandle<XrHandTrackerEXT>(handle);
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
Fake_xrDestroyHandTrackerEXT(XrHandTrackerEXT tracker) {
  gRuntime.mHandTrackers.erase(FromHandle(tracker));
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrLocateHandJointsEXT(
  XrHandTrackerEXT tracker,
  const XrHandJointsLocateInfoEXT* info,
  XrHandJointLocationsEXT* locations) {
  const auto it = gRuntime.mHandTrackers.find(FromHandle(tracker));
  if (it == gRuntime.mHandTrackers.end()) {
    return XR_ERROR_HANDLE_INVALID;
  }
  const auto& hand = gRuntime.mHands[it->second == XR_HAND_LEFT_EXT ? 0 : 1];
  locations->isActive = hand.mIsTracked ? XR_TRUE : XR_FALSE;
  const auto base = SpacePose(info->baseSpace);
  const auto count = std::min<uint32_t>(locations->jointCount, JointCount);
  for (uint32_t i = 0; i < count; ++i) {
    auto& out = locations->jointLocations[i];
    if (!hand.mIsTracked) {
      out.locationFlags = 0;
      continue;
    }
    const auto& joint = hand.mJoints[i];
    out.locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
      | XR_SPACE_LOCATION_POSITION_VALID_BIT
      | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT
      | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    out.pose = ToXr(Relative({joint.mPosition, joint.mOrientation}, base));
    out.radius = joint.mRadius;
  }
  return XR_SUCCESS;
}

// Reads view 0 of the hand layer: counts drawn pixels, and optionally saves it
void InspectHandLayer(const XrCompositionLayerProjection& layer) {
  const auto& view = layer.views[0];
  const auto& fake
    = gRuntime.mSwapchains.at(FromHandle(view.subImage.swapchain));
  const auto& rect = view.subImage.imageRect;
  const auto width = static_cast<uint32_t>(rect.extent.width);
  const auto height = static_cast<uint32_t>(rect.extent.height);

  const D3D11_TEXTURE2D_DESC desc {
    .Width = width,
    .Height = height,
    .MipLevels = 1,
    .ArraySize = 1,
    .Format = fake.mTextureFormat,
    .SampleDesc = {1, 0},
    .Usage = D3D11_USAGE_STAGING,
    .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
  };
  winrt::com_ptr<ID3D11Texture2D> staging;
  winrt::check_hresult(
    gRuntime.mDevice->CreateTexture2D(&desc, nullptr, staging.put()));
  const D3D11_BOX box {
    .left = static_cast<UINT>(rect.offset.x),
    .top = static_cast<UINT>(rect.offset.y),
    .front = 0,
    .right = static_cast<UINT>(rect.offset.x) + width,
    .bottom = static_cast<UINT>(rect.offset.y) + height,
    .back = 1,
  };
  gRuntime.mContext->CopySubresourceRegion(
    staging.get(), 0, 0, 0, 0, fake.mImages.at(fake.mReleased).get(), 0, &box);

  D3D11_MAPPED_SUBRESOURCE mapped {};
  winrt::check_hresult(
    gRuntime.mContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
  const bool isBGRA = (fake.mTextureFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS);
  int coverage = 0;
  int left = static_cast<int>(width);
  int top = static_cast<int>(height);
  int right = -1;
  int bottom = -1;
  std::vector<uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const auto row = static_cast<const uint8_t*>(mapped.pData)
      + (static_cast<std::size_t>(y) * mapped.RowPitch);
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* src = row + (x * 4);
      const uint8_t b = isBGRA ? src[0] : src[2];
      const uint8_t g = src[1];
      const uint8_t r = isBGRA ? src[2] : src[0];
      const float alpha = src[3] / 255.0f;
      if (src[3] > 0) {
        ++coverage;
        left = std::min(left, static_cast<int>(x));
        right = std::max(right, static_cast<int>(x));
        top = std::min(top, static_cast<int>(y));
        bottom = std::max(bottom, static_cast<int>(y));
      }
      // Premultiplied, over a dark grey background
      uint8_t* dst = &bgra[(static_cast<std::size_t>(y) * width + x) * 4];
      dst[0] = static_cast<uint8_t>(std::min(255.0f, b + (52 * (1 - alpha))));
      dst[1] = static_cast<uint8_t>(std::min(255.0f, g + (44 * (1 - alpha))));
      dst[2] = static_cast<uint8_t>(std::min(255.0f, r + (40 * (1 - alpha))));
      dst[3] = 255;
    }
  }
  gRuntime.mContext->Unmap(staging.get(), 0);
  gRuntime.mHandLayerCoverage = coverage;

  // Where the right index fingertip really is, in the left eye's image
  // (world space, so a wrong space transform shows up here; XR-12)
  if (gRuntime.mCheckHandPlacement && gRuntime.mHands[1].mIsTracked) {
    const auto tip = LeftEyePixel(
      gRuntime.mHands[1][Joint::IndexTip].mPosition, view.fov, width, height);
    ++gRuntime.mHandPlacementChecks;
    bool inside = false;
    if (tip && right >= 0) {
      const auto [px, py] = *tip;
      constexpr float margin = 10;
      inside = px >= left - margin && px <= right + margin && py >= top - margin
        && py <= bottom + margin;
    }
    if (!inside) {
      ++gRuntime.mHandPlacementMisses;
    }
  }

  // Reaching behind a panel: drawn where the hand is, not pushed back out
  // to the panel's surface as a fingertip pressing it would be
  if (
    gRuntime.mCheckDrawnWhereItIs && gRuntime.mHands[1].mIsTracked && right >= 0
    && gRuntime.mQuad) {
    const auto& hand = gRuntime.mHands[1];
    const auto panel = WorldPanel(*gRuntime.mQuad);
    const auto& tip = hand[Joint::IndexTip];
    const auto onPanel
      = HandInteraction::ProjectOntoPanel(tip.mPosition, panel, HMDPose.mPosition);
    auto limited = hand;
    const float shift = tip.mRadius - onPanel.mDistance;
    for (auto& joint: limited.mJoints) {
      joint.mPosition += onPanel.mFrontNormal * shift;
    }
    const auto real = HandMiddle(hand, view.fov, width, height);
    const auto pushed = HandMiddle(limited, view.fov, width, height);
    if (real && pushed) {
      const float x = static_cast<float>(left + right) / 2;
      const float y = static_cast<float>(top + bottom) / 2;
      ++gRuntime.mDrawnChecks;
      gRuntime.mDrawnWorstOffset = std::max(
        gRuntime.mDrawnWorstOffset, std::hypot(x - real->first, y - real->second));
      gRuntime.mDrawnNearestLimited = std::min(
        gRuntime.mDrawnNearestLimited,
        std::hypot(x - pushed->first, y - pushed->second));
    }
  }

  if (gRuntime.mSaveHandLayerTo) {
    TestUtilities::SavePNG(*gRuntime.mSaveHandLayerTo, width, height, bgra);
    std::printf(
      "  saved what the hand layer shows the left eye: %s\n",
      gRuntime.mSaveHandLayerTo->string().c_str());
    gRuntime.mSaveHandLayerTo.reset();
  }
}

// Average brightness (0-255) of a small area in the middle of a panel
std::optional<float> PanelBrightness(const XrCompositionLayerQuad& quad) {
  const auto it
    = gRuntime.mSwapchains.find(FromHandle(quad.subImage.swapchain));
  if (it == gRuntime.mSwapchains.end()) {
    return std::nullopt;
  }
  const auto& fake = it->second;
  const auto& rect = quad.subImage.imageRect;
  constexpr UINT boxSize = 16;
  const auto x
    = static_cast<UINT>(rect.offset.x + (rect.extent.width / 2) - (boxSize / 2));
  const auto y = static_cast<UINT>(
    rect.offset.y + (rect.extent.height / 2) - (boxSize / 2));
  const D3D11_TEXTURE2D_DESC desc {
    .Width = boxSize,
    .Height = boxSize,
    .MipLevels = 1,
    .ArraySize = 1,
    .Format = fake.mTextureFormat,
    .SampleDesc = {1, 0},
    .Usage = D3D11_USAGE_STAGING,
    .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
  };
  winrt::com_ptr<ID3D11Texture2D> staging;
  winrt::check_hresult(
    gRuntime.mDevice->CreateTexture2D(&desc, nullptr, staging.put()));
  const D3D11_BOX box {x, y, 0, x + boxSize, y + boxSize, 1};
  gRuntime.mContext->CopySubresourceRegion(
    staging.get(), 0, 0, 0, 0, fake.mImages.at(fake.mReleased).get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped {};
  winrt::check_hresult(
    gRuntime.mContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
  float total = 0;
  for (UINT row = 0; row < boxSize; ++row) {
    const auto pixels = static_cast<const uint8_t*>(mapped.pData)
      + (static_cast<std::size_t>(row) * mapped.RowPitch);
    for (UINT col = 0; col < boxSize; ++col) {
      const uint8_t* p = pixels + (col * 4);
      total += (p[0] + p[1] + p[2]) / 3.0f;
    }
  }
  gRuntime.mContext->Unmap(staging.get(), 0);
  return total / (boxSize * boxSize);
}

// Saves a panel layer's image as a PNG, over dark grey (M5b: the keyboard,
// for looking at)
void SaveQuad(const XrCompositionLayerQuad& quad, const std::filesystem::path& path) {
  const auto it = gRuntime.mSwapchains.find(FromHandle(quad.subImage.swapchain));
  if (it == gRuntime.mSwapchains.end()) {
    return;
  }
  const auto& fake = it->second;
  const auto& rect = quad.subImage.imageRect;
  const auto width = static_cast<uint32_t>(rect.extent.width);
  const auto height = static_cast<uint32_t>(rect.extent.height);
  const D3D11_TEXTURE2D_DESC desc {
    .Width = width,
    .Height = height,
    .MipLevels = 1,
    .ArraySize = 1,
    .Format = fake.mTextureFormat,
    .SampleDesc = {1, 0},
    .Usage = D3D11_USAGE_STAGING,
    .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
  };
  winrt::com_ptr<ID3D11Texture2D> staging;
  if (FAILED(gRuntime.mDevice->CreateTexture2D(&desc, nullptr, staging.put()))) {
    return;
  }
  const D3D11_BOX box {static_cast<UINT>(rect.offset.x), static_cast<UINT>(rect.offset.y), 0,
    static_cast<UINT>(rect.offset.x) + width, static_cast<UINT>(rect.offset.y) + height, 1};
  gRuntime.mContext->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, fake.mImages.at(fake.mReleased).get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped {};
  if (FAILED(gRuntime.mContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return;
  }
  const bool isBGRA = (fake.mTextureFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS);
  std::vector<uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const auto row = static_cast<const uint8_t*>(mapped.pData) + (static_cast<std::size_t>(y) * mapped.RowPitch);
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* src = row + (x * 4);
      uint8_t* out = bgra.data() + ((static_cast<std::size_t>(y) * width + x) * 4);
      const int under = (255 - src[3]) * 40 / 255;// premultiplied, over grey 40
      out[0] = static_cast<uint8_t>(std::min(255, (isBGRA ? src[0] : src[2]) + under));
      out[1] = static_cast<uint8_t>(std::min(255, src[1] + under));
      out[2] = static_cast<uint8_t>(std::min(255, (isBGRA ? src[2] : src[0]) + under));
      out[3] = 255;
    }
  }
  gRuntime.mContext->Unmap(staging.get(), 0);
  TestUtilities::SavePNG(path, width, height, bgra);
}

// Has the click target's orange click marker appeared on the panel?
bool PanelShowsClickMarker(const XrCompositionLayerQuad& quad) {
  const auto it
    = gRuntime.mSwapchains.find(FromHandle(quad.subImage.swapchain));
  if (it == gRuntime.mSwapchains.end()) {
    return false;
  }
  const auto& fake = it->second;
  const auto& rect = quad.subImage.imageRect;
  constexpr UINT boxSize = 5;
  const auto x = static_cast<UINT>(
    rect.offset.x + (gRuntime.mWatchU * rect.extent.width) - (boxSize / 2));
  const auto y = static_cast<UINT>(
    rect.offset.y + (gRuntime.mWatchV * rect.extent.height) - (boxSize / 2));

  const D3D11_TEXTURE2D_DESC desc {
    .Width = boxSize,
    .Height = boxSize,
    .MipLevels = 1,
    .ArraySize = 1,
    .Format = fake.mTextureFormat,
    .SampleDesc = {1, 0},
    .Usage = D3D11_USAGE_STAGING,
    .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
  };
  winrt::com_ptr<ID3D11Texture2D> staging;
  winrt::check_hresult(
    gRuntime.mDevice->CreateTexture2D(&desc, nullptr, staging.put()));
  const D3D11_BOX box {x, y, 0, x + boxSize, y + boxSize, 1};
  gRuntime.mContext->CopySubresourceRegion(
    staging.get(), 0, 0, 0, 0, fake.mImages.at(fake.mReleased).get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped {};
  winrt::check_hresult(
    gRuntime.mContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
  const bool isBGRA = (fake.mTextureFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS);
  int orange = 0;
  for (UINT row = 0; row < boxSize; ++row) {
    const auto pixels = static_cast<const uint8_t*>(mapped.pData)
      + (static_cast<std::size_t>(row) * mapped.RowPitch);
    for (UINT col = 0; col < boxSize; ++col) {
      const uint8_t* p = pixels + (col * 4);
      const int r = isBGRA ? p[2] : p[0];
      const int g = p[1];
      const int b = isBGRA ? p[0] : p[2];
      // By hue rather than brightness, so a panel's tint or brightness
      // setting doesn't hide it; the marker is RGB(255, 120, 60) at full
      // brightness
      if (r >= 60 && r * 10 > g * 16 && r * 10 > b * 25 && g > b) {
        ++orange;
      }
    }
  }
  gRuntime.mContext->Unmap(staging.get(), 0);
  return orange >= (boxSize * boxSize) / 2;
}

// Share of the game's field of view (both eyes, in tangent space) that a
// projection layer covers
double ProjectionCoverage(
  const XrCompositionLayerProjection& layer,
  const XrFovf& gameFov) {
  const auto area = [](const XrFovf& fov) {
    return (std::tan(fov.angleRight) - std::tan(fov.angleLeft))
      * (std::tan(fov.angleUp) - std::tan(fov.angleDown));
  };
  double total = 0;
  for (uint32_t i = 0; i < layer.viewCount; ++i) {
    const auto& fov = layer.views[i].fov;
    const XrFovf clipped {
      std::max(fov.angleLeft, gameFov.angleLeft),
      std::min(fov.angleRight, gameFov.angleRight),
      std::min(fov.angleUp, gameFov.angleUp),
      std::max(fov.angleDown, gameFov.angleDown),
    };
    if (
      clipped.angleRight > clipped.angleLeft
      && clipped.angleUp > clipped.angleDown) {
      total += area(clipped) / area(gameFov);
    }
  }
  return layer.viewCount ? total / layer.viewCount : 0;
}

XRAPI_ATTR XrResult XRAPI_CALL
Fake_xrEndFrame(XrSession, const XrFrameEndInfo* info) {
  if (gRuntime.mBench) {
    const XrFovf gameFov {-0.9f, 0.9f, 0.9f, -0.9f};
    for (uint32_t i = 0; i < info->layerCount; ++i) {
      const auto layer = info->layers[i];
      if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        gRuntime.mQuad = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
        continue;
      }
      if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        continue;
      }
      const auto& projection
        = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
      if (gRuntime.mSwapchains.contains(
            FromHandle(projection.views[0].subImage.swapchain))) {
        ++gRuntime.mBenchHandFrames;
        gRuntime.mBenchHandCoverage += ProjectionCoverage(projection, gameFov);
      }
    }
    return XR_SUCCESS;
  }
  if (
    gRuntime.mWatchPanel && gRuntime.mQuad
    && !gRuntime.mPanelChangedAt
    && PanelShowsClickMarker(*gRuntime.mQuad)) {
    gRuntime.mPanelChangedAt = NowMilliseconds();
  }
  gRuntime.mHandLayerThisFrame = false;
  gRuntime.mQuadCount = 0;
  gRuntime.mQuads.clear();
  gRuntime.mKeyboardQuad.reset();
  {
    std::optional<float> previous;
    bool outOfOrder = false;
    int quads = 0;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
      if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
        continue;
      }
      ++quads;
      const auto& quad
        = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
      const auto distance
        = HandInteraction::DistanceToPanel(HMDPose.mPosition, WorldPanel(quad));
      if (previous && distance > *previous + 0.001f) {
        outOfOrder = true;
      }
      previous = distance;
    }
    if (quads > 1) {
      ++gRuntime.mFramesWithMultipleQuads;
      if (outOfOrder) {
        ++gRuntime.mFramesOutOfOrder;
      }
    }
  }
  for (uint32_t i = 0; i < info->layerCount; ++i) {
    const auto layer = info->layers[i];
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
      ++gRuntime.mQuadCount;
      gRuntime.mQuads.push_back(*reinterpret_cast<const XrCompositionLayerQuad*>(layer));
      if (!gRuntime.mQuad) {
        const auto& quad
          = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
        std::printf(
          "Panel layer: %.2fm x %.2fm at (%.2f, %.2f, %.2f)\n",
          quad.size.width,
          quad.size.height,
          quad.pose.position.x,
          quad.pose.position.y,
          quad.pose.position.z);
      }
      gRuntime.mQuad = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
      if (gRuntime.mQuad->size.width > 2.2f * gRuntime.mQuad->size.height) {
        gRuntime.mKeyboardQuad = gRuntime.mQuad;
      } else {
        gRuntime.mPanelQuad = gRuntime.mQuad;
      }
      continue;
    }
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      const auto& projection
        = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
      // The game's own layer uses a swapchain we didn't create
      if (gRuntime.mSwapchains.contains(
            FromHandle(projection.views[0].subImage.swapchain))) {
        gRuntime.mHandLayerThisFrame = true;
        const auto handle = FromHandle(projection.views[0].subImage.swapchain);
        if (gRuntime.mFailedHandSwapchain != 0) {
          ++gRuntime.mHandLayerFramesAfterFailure;
          gRuntime.mNewHandSwapchainAfterFailure
            |= handle != gRuntime.mFailedHandSwapchain;
        }
        gRuntime.mHandSwapchain = handle;
        // Virtual Desktop (VDXR) applies one projection layer's field of view
        // to the others, so the hand layer must use exactly the game's, over
        // its whole image
        const XrCompositionLayerProjection* game = nullptr;
        for (uint32_t j = 0; j < info->layerCount; ++j) {
          const auto other = info->layers[j];
          if (
            other->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION
            && other != layer) {
            game = reinterpret_cast<const XrCompositionLayerProjection*>(other);
            break;
          }
        }
        const auto& fake = gRuntime.mSwapchains.at(
          FromHandle(projection.views[0].subImage.swapchain));
        bool matches = game && game->viewCount == projection.viewCount;
        for (uint32_t v = 0; matches && v < projection.viewCount; ++v) {
          const auto& ours = projection.views[v];
          const auto& theirs = game->views[v];
          matches = std::memcmp(&ours.fov, &theirs.fov, sizeof(XrFovf)) == 0
            && std::memcmp(&ours.pose, &theirs.pose, sizeof(XrPosef)) == 0
            && ours.subImage.imageRect.extent.width
              == static_cast<int32_t>(fake.mInfo.width / projection.viewCount)
            && ours.subImage.imageRect.extent.height
              == static_cast<int32_t>(fake.mInfo.height);
        }
        if (!matches) {
          ++gRuntime.mHandLayerViewMismatches;
        }
        InspectHandLayer(projection);
      }
    }
  }
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrGetInstanceProcAddr(
  XrInstance,
  const char* name,
  PFN_xrVoidFunction* function);

const std::map<std::string_view, PFN_xrVoidFunction>& FakeFunctions() {
#define FN(x) {#x, reinterpret_cast<PFN_xrVoidFunction>(&Fake_##x)}
  static const std::map<std::string_view, PFN_xrVoidFunction> functions {
    FN(xrGetInstanceProcAddr),
    FN(xrEnumerateInstanceExtensionProperties),
    FN(xrGetInstanceProperties),
    FN(xrGetSystemProperties),
    FN(xrCreateSession),
    FN(xrDestroySession),
    FN(xrDestroyInstance),
    FN(xrCreateReferenceSpace),
    FN(xrDestroySpace),
    FN(xrLocateSpace),
    FN(xrPollEvent),
    FN(xrEnumerateSwapchainFormats),
    FN(xrCreateSwapchain),
    FN(xrDestroySwapchain),
    FN(xrEnumerateSwapchainImages),
    FN(xrAcquireSwapchainImage),
    FN(xrWaitSwapchainImage),
    FN(xrReleaseSwapchainImage),
    FN(xrEndFrame),
    FN(xrCreateHandTrackerEXT),
    FN(xrDestroyHandTrackerEXT),
    FN(xrLocateHandJointsEXT),
  };
#undef FN
  return functions;
}

XRAPI_ATTR XrResult XRAPI_CALL Fake_xrGetInstanceProcAddr(
  XrInstance,
  const char* name,
  PFN_xrVoidFunction* function) {
  if (gRuntime.mEarlyLookupsFail && !gRuntime.mInstanceCreated) {
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  const auto& functions = FakeFunctions();
  const auto it = functions.find(name);
  if (it == functions.end()) {
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  *function = it->second;
  return XR_SUCCESS;
}

///// The script /////

struct Checks {
  int mFailures {0};
  void Check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
    if (!ok) {
      ++mFailures;
    }
  }
};

struct Stats {
  int mFrames {0};
  int mHandLayerFrames {0};
};

enum class Step {
  Waiting,
  NoHands,
  FlatHand,
  Pointing,
  PokeIn,
  Touching,
  PullBack,
  Hovering,
  FlatSweep,
  ScrollHover,
  ScrollTouch,
  ScrollDrag,
  ScrollRelease,
  Done,
};

const char* StepName(Step step) {
  switch (step) {
    case Step::Waiting:
      return "waiting for the panel";
    case Step::NoHands:
      return "no hands";
    case Step::FlatHand:
      return "flat hand in front of the panel";
    case Step::Pointing:
      return "pointing, 8cm in front";
    case Step::PokeIn:
      return "poking in";
    case Step::Touching:
      return "touching (1.5cm through)";
    case Step::PullBack:
      return "pulling back";
    case Step::Hovering:
      return "hovering, 8cm in front";
    case Step::FlatSweep:
      return "flat hand sweeping through the panel";
    case Step::ScrollHover:
      return "two fingers out, 8cm in front";
    case Step::ScrollTouch:
      return "two fingers touching";
    case Step::ScrollDrag:
      return "two fingers dragging down (scrolling)";
    case Step::ScrollRelease:
      return "two fingers pulling back";
    case Step::Done:
      return "done";
  }
  return "?";
}

struct Timeline {
  Step mStep;
  double mStart;
  double mEnd;
};

constexpr std::array<Timeline, 13> Script {{
  {Step::NoHands, 0.0, 1.0},
  {Step::FlatHand, 1.0, 2.0},
  {Step::Pointing, 2.0, 3.0},
  {Step::PokeIn, 3.0, 3.4},
  {Step::Touching, 3.4, 3.7},
  {Step::PullBack, 3.7, 4.1},
  {Step::Hovering, 4.1, 5.0},
  {Step::FlatSweep, 5.0, 6.0},
  {Step::ScrollHover, 6.0, 6.5},
  {Step::ScrollTouch, 6.5, 6.8},
  {Step::ScrollDrag, 6.8, 7.4},
  {Step::ScrollRelease, 7.4, 7.7},
  {Step::Done, 7.7, 8.5},
}};

// Where the fingertip goes: (u, v) across the panel, and how far in front
Vector3 OnPanel(
  const XrCompositionLayerQuad& quad,
  float u,
  float v,
  float distance,
  Vector3* normalOut = nullptr,
  Vector3* upOut = nullptr) {
  // In the world, wherever the game's space is
  const auto world = QuadWorldPose(quad);
  const auto& orientation = world.mOrientation;
  const Vector3 center = world.mPosition;
  const auto right = Vector3::Transform(Vector3::UnitX, orientation);
  const auto up = Vector3::Transform(Vector3::UnitY, orientation);
  auto normal = Vector3::Transform(Vector3::UnitZ, orientation);
  if ((HMDPose.mPosition - center).Dot(normal) < 0) {
    normal = -normal;
  }
  if (normalOut) {
    *normalOut = normal;
  }
  if (upOut) {
    *upOut = up;
  }
  return center + (right * ((u - 0.5f) * quad.size.width))
    + (up * ((0.5f - v) * quad.size.height)) + (normal * distance);
}

HandJoints HandAt(
  Pose pose,
  const XrCompositionLayerQuad& quad,
  float u,
  float v,
  float distance) {
  Vector3 normal;
  Vector3 up;
  const auto tip = OnPanel(quad, u, v, distance, &normal, &up);
  // Fingers into the panel, back of the hand toward its top edge
  const Vector3 forward = -normal;
  const Vector3 back = up;
  const HandBasis basis {back.Cross(forward), forward, back};
  auto hand = MakeHand(pose, basis, tip);
  if (pose == Pose::TwoFingers) {
    // Scrolling touches between the two fingertips
    const auto midpoint
      = (hand[Joint::IndexTip].mPosition + hand[Joint::MiddleTip].mPosition)
      * 0.5f;
    for (auto& joint: hand.mJoints) {
      joint.mPosition += tip - midpoint;
    }
  }
  return hand;
}

// The click target's position on the panel
constexpr float TargetU = 0.5f;
constexpr float TargetV = 0.55f;

void ScriptHands(Step step, double progress, const XrCompositionLayerQuad& quad) {
  auto& right = gRuntime.mHands[1];
  gRuntime.mHands[0] = {};
  const auto lerp = [&](float a, float b) {
    return a + ((b - a) * static_cast<float>(progress));
  };
  switch (step) {
    case Step::Waiting:
    case Step::NoHands:
    case Step::Done:
      right = {};
      return;
    case Step::FlatHand:
      right = HandAt(Pose::Flat, quad, TargetU, TargetV, 0.12f);
      return;
    case Step::Pointing:
    case Step::Hovering:
      right = HandAt(Pose::Point, quad, TargetU, TargetV, 0.08f);
      return;
    case Step::PokeIn:
      right = HandAt(Pose::Point, quad, TargetU, TargetV, lerp(0.08f, -0.015f));
      return;
    case Step::Touching:
      right = HandAt(Pose::Point, quad, TargetU, TargetV, -0.015f);
      return;
    case Step::PullBack:
      right = HandAt(Pose::Point, quad, TargetU, TargetV, lerp(-0.015f, 0.08f));
      return;
    case Step::FlatSweep:
      right = HandAt(
        Pose::Flat, quad, lerp(0.2f, 0.8f), TargetV, lerp(0.06f, -0.04f));
      return;
    // Drag down 40% of the panel's height
    case Step::ScrollHover:
      right = HandAt(Pose::TwoFingers, quad, TargetU, 0.4f, 0.08f);
      return;
    case Step::ScrollTouch:
      right = HandAt(Pose::TwoFingers, quad, TargetU, 0.4f, lerp(0.08f, -0.005f));
      return;
    case Step::ScrollDrag:
      right = HandAt(Pose::TwoFingers, quad, TargetU, lerp(0.4f, 0.8f), -0.005f);
      return;
    case Step::ScrollRelease:
      right = HandAt(Pose::TwoFingers, quad, TargetU, 0.8f, lerp(-0.005f, 0.08f));
      return;
  }
}

///// '--gestures': the rest of doc 11's hand scenarios (added in M4c) /////

enum class Scenario {
  AngledPoke,
  HoldDrag,
  Scroll,
  Gap300,
  Gap100,
  ReachBehind,
  End,
};

struct GestureStep {
  const char* mName;
  Scenario mScenario;
  double mSeconds;
  // The right hand at `t` (0 to 1) through the step; untracked if not there
  std::function<HandJoints(float t, const XrCompositionLayerQuad& quad)> mHand;
  // The hand layer's next image wait fails once, as the step starts
  bool mFailHandWait {false};
  // The drawn hand is checked: where the hand is, not limited to the panel
  bool mCheckDrawn {false};
};

// The angled poke comes in at about 39 degrees off straight (a slope of
// 0.8, as the imported test's), moving down the panel, and meets it here
constexpr float AngledU = 0.5f;
constexpr float AngledV = 0.55f;
constexpr float AngledSlope = 0.8f;
// How far the two-finger scroll drags. Doc 11's 36 mm is exactly 3 notches
// of 12 mm, and only whole notches are sent, so it gave 2 or 3 depending on
// rounding; half a millimetre past it gives 3 (measured in M4c: filtering
// loses under 0.5 mm of the drag)
constexpr float ScrollMeters = 0.0365f;

std::vector<GestureStep> GestureSteps() {
  const auto lerp = [](float a, float b, float t) { return a + ((b - a) * t); };
  // On the line that meets the panel at the aimed point, `d` in front
  const auto angled = [](float d, const XrCompositionLayerQuad& q) {
    return HandAt(Pose::Point, q, AngledU, AngledV - (AngledSlope * d / q.size.height), d);
  };
  const auto point = [](float u, float v, float d) {
    return [=](float, const XrCompositionLayerQuad& q) { return HandAt(Pose::Point, q, u, v, d); };
  };
  const auto lost = [](float, const XrCompositionLayerQuad&) { return HandJoints {}; };
  // A drag with a tracking gap in it: poke at (u, v), drag 20 mm right,
  // lose tracking for `gap` seconds, come back still touching, pull back
  const auto gapDrag = [&](Scenario scenario, float u, float v, double gap) {
    const auto dragged = [=](const XrCompositionLayerQuad& q) { return u + (0.020f / q.size.width); };
    return std::vector<GestureStep> {
      {"hovering in front", scenario, 0.5, point(u, v, 0.08f)},
      {"poking in", scenario, 0.3,
        [=](float t, const auto& q) { return HandAt(Pose::Point, q, u, v, lerp(0.08f, -0.005f, t)); }},
      {"dragging 20 mm right", scenario, 0.4,
        [=](float t, const auto& q) { return HandAt(Pose::Point, q, lerp(u, dragged(q), t), v, -0.005f); }},
      {gap > 0.2 ? "tracking lost for 300 ms" : "tracking lost for 100 ms", scenario, gap, lost},
      {"back, still touching", scenario, 0.3,
        [=](float, const auto& q) { return HandAt(Pose::Point, q, dragged(q), v, -0.005f); }},
      {"pulling back", scenario, 0.3,
        [=](float t, const auto& q) { return HandAt(Pose::Point, q, dragged(q), v, lerp(-0.005f, 0.08f, t)); }},
    };
  };

  std::vector<GestureStep> steps {
    // Angled poke (doc 11): the click within 1 mm of where the finger meets
    // the panel
    {"angled poke: hovering on its line, 8 cm out", Scenario::AngledPoke, 0.6,
      [=](float, const auto& q) { return angled(0.08f, q); }},
    {"angled poke: in along it to 1 cm through", Scenario::AngledPoke, 0.4,
      [=](float t, const auto& q) { return angled(lerp(0.08f, -0.01f, t), q); }},
    {"angled poke: back out the way it came", Scenario::AngledPoke, 0.3,
      [=](float t, const auto& q) { return angled(lerp(-0.01f, 0.08f, t), q); }},
    // Hold 1 s, then drag 40 mm: one down, moves, one up, no early release.
    // The hand layer's image wait fails once while hovering (XR-15)
    {"hold and drag: hovering in front (the hand layer's next image wait fails)", Scenario::HoldDrag, 0.6,
      point(0.3f, 0.45f, 0.08f), true},
    {"hold and drag: poking in", Scenario::HoldDrag, 0.3,
      [=](float t, const auto& q) { return HandAt(Pose::Point, q, 0.3f, 0.45f, lerp(0.08f, -0.005f, t)); }},
    {"hold and drag: holding still for 1 s", Scenario::HoldDrag, 1.0, point(0.3f, 0.45f, -0.005f)},
    {"hold and drag: dragging 40 mm right", Scenario::HoldDrag, 0.8,
      [=](float t, const auto& q) {
        return HandAt(Pose::Point, q, lerp(0.3f, 0.3f + (0.040f / q.size.width), t), 0.45f, -0.005f);
      }},
    {"hold and drag: holding at the end", Scenario::HoldDrag, 0.2,
      [=](float, const auto& q) { return HandAt(Pose::Point, q, 0.3f + (0.040f / q.size.width), 0.45f, -0.005f); }},
    {"hold and drag: pulling back", Scenario::HoldDrag, 0.3,
      [=](float t, const auto& q) {
        return HandAt(Pose::Point, q, 0.3f + (0.040f / q.size.width), 0.45f, lerp(-0.005f, 0.08f, t));
      }},
    // Two-finger scroll down 36 mm: 3 wheel notches, natural direction
    {"scroll: two fingers out, in front", Scenario::Scroll, 0.5,
      [=](float, const auto& q) { return HandAt(Pose::TwoFingers, q, 0.5f, 0.3f, 0.08f); }},
    {"scroll: two fingers touching", Scenario::Scroll, 0.3,
      [=](float t, const auto& q) { return HandAt(Pose::TwoFingers, q, 0.5f, 0.3f, lerp(0.08f, -0.005f, t)); }},
    {"scroll: dragging down 36.5 mm", Scenario::Scroll, 0.7,
      [=](float t, const auto& q) {
        return HandAt(Pose::TwoFingers, q, 0.5f, lerp(0.3f, 0.3f + (ScrollMeters / q.size.height), t), -0.005f);
      }},
    {"scroll: holding", Scenario::Scroll, 0.15,
      [=](float, const auto& q) { return HandAt(Pose::TwoFingers, q, 0.5f, 0.3f + (ScrollMeters / q.size.height), -0.005f); }},
    {"scroll: pulling back", Scenario::Scroll, 0.3,
      [=](float t, const auto& q) {
        return HandAt(Pose::TwoFingers, q, 0.5f, 0.3f + (ScrollMeters / q.size.height), lerp(-0.005f, 0.08f, t));
      }},
  };
  for (auto step: gapDrag(Scenario::Gap300, 0.25f, 0.75f, 0.3)) {
    steps.push_back(step);
  }
  for (auto step: gapDrag(Scenario::Gap100, 0.6f, 0.75f, 0.1)) {
    steps.push_back(step);
  }
  // Reach behind a panel: 0 clicks; drawn where it is (more than 8 cm
  // behind: not touch-limited)
  constexpr float behind = -0.12f;
  steps.insert(steps.end(), {
    {"reach behind: beside the panel, 12 cm behind it", Scenario::ReachBehind, 0.4, point(-0.35f, 0.5f, behind)},
    {"reach behind: sliding in behind the panel", Scenario::ReachBehind, 0.6,
      [=](float t, const auto& q) { return HandAt(Pose::Point, q, lerp(-0.35f, 0.5f, t), 0.5f, behind); }},
    {"reach behind: behind the panel", Scenario::ReachBehind, 0.5, point(0.5f, 0.5f, behind), false, true},
    {"reach behind: sliding out", Scenario::ReachBehind, 0.6,
      [=](float t, const auto& q) { return HandAt(Pose::Point, q, lerp(0.5f, 1.35f, t), 0.5f, behind); }},
    {"done", Scenario::End, 0.8, lost},
  });
  return steps;
}

// A click-target log line with a time: "DOWN x y t", "UP x y t", "DOUBLE x y t"
struct Click {
  std::string mKind;
  double mX {};
  double mY {};
  double mAt {};
};

std::vector<Click> TimedClicks(const std::vector<std::string>& lines) {
  std::vector<Click> clicks;
  for (const auto& line: lines) {
    std::istringstream fields(line);
    Click click;
    if (
      (line.starts_with("DOWN ") || line.starts_with("UP ") || line.starts_with("DOUBLE "))
      && (fields >> click.mKind >> click.mX >> click.mY >> click.mAt)) {
      clicks.push_back(click);
    }
  }
  return clicks;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    lines.push_back(line);
  }
  return lines;
}

int CountPrefix(const std::vector<std::string>& lines, std::string_view prefix) {
  return static_cast<int>(std::ranges::count_if(
    lines, [prefix](const auto& line) { return line.starts_with(prefix); }));
}

}// namespace

int main(int rawArgc, char** rawArgv) {
  // Named options can go anywhere; what's left is positional
  std::string negotiateExport {"xrNegotiateLoaderApiLayerInterface"};
  std::string layerName {"XR_APILAYER_TEST_layer"};
  bool gestures = false;
  bool depthOrder = false;
  bool recenter = false;
  bool quickMenu = false;
  std::vector<char*> positional;
  for (int i = 0; i < rawArgc; ++i) {
    const std::string_view arg {rawArgv[i]};
    if (arg == "--negotiate-export" && i + 1 < rawArgc) {
      negotiateExport = rawArgv[++i];
    } else if (arg == "--layer-name" && i + 1 < rawArgc) {
      layerName = rawArgv[++i];
    } else if (arg == "--early-lookups-fail") {
      gRuntime.mEarlyLookupsFail = true;
    } else if (arg == "--no-hand-tracking") {
      gRuntime.mNoHandTracking = true;
    } else if (arg == "--stage-offset") {
      gRuntime.mStageOffset = true;
      gRuntime.mStage = {
        {0.4f, -1.1f, 0.7f},
        Quaternion::CreateFromAxisAngle(Vector3::UnitY, 0.6f),
      };
    } else if (arg == "--gestures") {
      gestures = true;
    } else if (arg == "--depth-order") {
      depthOrder = true;
    } else if (arg == "--recenter") {
      recenter = true;
    } else if (arg == "--quick-menu") {
      quickMenu = true;
    } else {
      positional.push_back(rawArgv[i]);
    }
  }
  const int argc = static_cast<int>(positional.size());
  char** argv = positional.data();
  if (argc < 3) {
    std::printf(
      "Usage: openxr-simulator [--negotiate-export <name>] "
      "[--layer-name <name>] [--early-lookups-fail] [--no-hand-tracking] "
      "[--stage-offset] [--gestures] [--depth-order] "
      "<layer DLL> <output folder> "
      "[click log | --watch <s> | --type-keys ... | --bench <s> [hover]]\n");
    return 1;
  }
  const std::filesystem::path layerPath {argv[1]};
  const std::filesystem::path outputDir {argv[2]};
  // '--watch <seconds>': no hands; report changes to the panel layers
  std::optional<double> watchSeconds;
  // '--bench <seconds> [hover]': measure the layer's GPU time per frame, and
  // what it gives the runtime to composite; optionally with a pointing hand
  // hovering in front of the panel
  std::optional<double> benchSeconds;
  bool benchHover = false;
  std::optional<std::filesystem::path> clickLog;
  // '--type-keys u,v;u,v;...': the right
  // index fingertip presses these points on the virtual keyboard, which must
  // already be showing (tap a text field first)
  std::vector<std::pair<float, float>> typeKeys;
  // '--type <text> <log>': tap the window panel, then type <text>'s letters
  std::string typeText;
  std::optional<std::filesystem::path> typeLog;
  if (argc > 5 && std::string_view {argv[3]} == "--type") {
    typeText = argv[4];
    typeLog = std::filesystem::path {argv[5]};
    // Where each letter is: its scan code on the US layout (H and I are
    // in the same place on QWERTY, QWERTZ and AZERTY), then that key's
    // middle in the keyboard's geometry (H and I are where they are on ISO
    // and ANSI alike; the function row is on by default)
    const auto us = KeyboardLayout::Layout::Load("00000409");
    const auto geometry = VirtualKeyboard::BuildGeometry({});
    for (const char c: typeText) {
      std::optional<std::pair<float, float>> place;
      for (KeyboardLayout::ScanCode scan = 0x02; us && scan <= 0x39 && !place; ++scan) {
        if (us->GetOutput(scan, {}).mText == std::wstring(1, static_cast<wchar_t>(c))) {
          if (const int key = VirtualKeyboard::FindKey(geometry, scan); key >= 0) {
            place = VirtualKeyboard::KeyCentre(geometry, key);
          }
        }
      }
      if (!place) {
        std::printf("FAIL: no key types '%c'\n", c);
        return 1;
      }
      typeKeys.push_back(*place);
    }
  } else if (argc > 4 && std::string_view {argv[3]} == "--type-keys") {
    std::stringstream list {argv[4]};
    std::string item;
    while (std::getline(list, item, ';')) {
      float u {}, v {};
      if (sscanf_s(item.c_str(), "%f,%f", &u, &v) == 2) {
        typeKeys.emplace_back(u, v);
      }
    }
  } else if (argc > 4 && std::string_view {argv[3]} == "--watch") {
    watchSeconds = std::atof(argv[4]);
  } else if (argc > 4 && std::string_view {argv[3]} == "--bench") {
    benchSeconds = std::atof(argv[4]);
    benchHover = (argc > 5 && std::string_view {argv[5]} == "hover");
  } else if (argc > 3) {
    clickLog = std::filesystem::path {argv[3]};
  }
  std::filesystem::create_directories(outputDir);
  winrt::check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

  Checks checks;

  // Hardware first; WARP on machines without a GPU (CI). Added in the port.
  HRESULT created = E_FAIL;
  for (const auto driver: {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    gRuntime.mDevice = nullptr;
    gRuntime.mContext = nullptr;
    created = D3D11CreateDevice(
      nullptr,
      driver,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      nullptr,
      0,
      D3D11_SDK_VERSION,
      gRuntime.mDevice.put(),
      nullptr,
      gRuntime.mContext.put());
    if (SUCCEEDED(created)) {
      break;
    }
  }
  winrt::check_hresult(created);

  // Load the layer, as the OpenXR loader would
  const auto layer = LoadLibraryW(layerPath.wstring().c_str());
  if (!layer) {
    std::printf("Failed to load %s\n", layerPath.string().c_str());
    return 1;
  }
  const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
    GetProcAddress(layer, negotiateExport.c_str()));
  if (!negotiate) {
    std::printf("The layer has no negotiation function\n");
    return 1;
  }
  XrNegotiateLoaderInfo loaderInfo {
    .structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
    .structVersion = XR_LOADER_INFO_STRUCT_VERSION,
    .structSize = sizeof(XrNegotiateLoaderInfo),
    .minInterfaceVersion = 1,
    .maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION,
    .minApiVersion = XR_MAKE_VERSION(1, 0, 0),
    .maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff),
  };
  XrNegotiateApiLayerRequest layerRequest {
    .structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
    .structVersion = XR_API_LAYER_INFO_STRUCT_VERSION,
    .structSize = sizeof(XrNegotiateApiLayerRequest),
  };
  if (XR_FAILED(negotiate(&loaderInfo, layerName.c_str(), &layerRequest))) {
    std::printf("Layer negotiation failed\n");
    return 1;
  }

  XrApiLayerNextInfo nextInfo {
    .structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO,
    .structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION,
    .structSize = sizeof(XrApiLayerNextInfo),
    .nextGetInstanceProcAddr = &Fake_xrGetInstanceProcAddr,
    .nextCreateApiLayerInstance = &Fake_xrCreateApiLayerInstance,
    .next = nullptr,
  };
  strcpy_s(nextInfo.layerName, layerName.c_str());
  XrApiLayerCreateInfo apiLayerInfo {
    .structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
    .structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION,
    .structSize = sizeof(XrApiLayerCreateInfo),
    .nextInfo = &nextInfo,
  };
  // The game only asks for D3D11; the layer should add hand tracking
  const char* gameExtensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo instanceInfo {
    .type = XR_TYPE_INSTANCE_CREATE_INFO,
    .enabledExtensionCount = 1,
    .enabledExtensionNames = gameExtensions,
  };
  strcpy_s(instanceInfo.applicationInfo.applicationName, "Simulated game");
  instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  XrInstance instance {};
  if (XR_FAILED(layerRequest.createApiLayerInstance(
        &instanceInfo, &apiLayerInfo, &instance))) {
    std::printf("xrCreateApiLayerInstance failed\n");
    return 1;
  }

  std::printf("Startup\n");
  if (gRuntime.mEarlyLookupsFail) {
    std::printf(
      "  (the next layer can't resolve functions before its instance "
      "exists)\n");
  }
  if (gRuntime.mNoHandTracking) {
    checks.Check(
      !std::ranges::contains(
        gRuntime.mEnabledExtensions, XR_EXT_HAND_TRACKING_EXTENSION_NAME),
      "without runtime hand tracking, the instance was still created, "
      "without XR_EXT_hand_tracking");
    return checks.mFailures == 0 ? 0 : 1;
  }
  checks.Check(
    std::ranges::contains(
      gRuntime.mEnabledExtensions, XR_EXT_HAND_TRACKING_EXTENSION_NAME),
    "the layer turned on XR_EXT_hand_tracking for a game that didn't ask for "
    "it");

  const auto layerFunction = [&](const char* name) {
    PFN_xrVoidFunction fn {nullptr};
    layerRequest.getInstanceProcAddr(instance, name, &fn);
    return fn;
  };
  const auto layerCreateSession
    = reinterpret_cast<PFN_xrCreateSession>(layerFunction("xrCreateSession"));
  const auto layerEndFrame
    = reinterpret_cast<PFN_xrEndFrame>(layerFunction("xrEndFrame"));
  const auto layerDestroySession
    = reinterpret_cast<PFN_xrDestroySession>(layerFunction("xrDestroySession"));
  const auto layerDestroyInstance = reinterpret_cast<PFN_xrDestroyInstance>(
    layerFunction("xrDestroyInstance"));
  const auto layerPollEvent = reinterpret_cast<PFN_xrPollEvent>(layerFunction("xrPollEvent"));

  XrGraphicsBindingD3D11KHR binding {
    .type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR,
    .device = gRuntime.mDevice.get(),
  };
  const XrSessionCreateInfo sessionInfo {
    .type = XR_TYPE_SESSION_CREATE_INFO,
    .next = &binding,
    .systemId = 1,
  };
  XrSession session {};
  if (XR_FAILED(layerCreateSession(instance, &sessionInfo, &session))) {
    std::printf("xrCreateSession failed\n");
    return 1;
  }

  // The game's own reference space
  XrSpace gameSpace {};
  const XrReferenceSpaceCreateInfo spaceInfo {
    .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
    .referenceSpaceType = gRuntime.mStageOffset ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL,
    .poseInReferenceSpace = ToXr({}),
  };
  Fake_xrCreateReferenceSpace(session, &spaceInfo, &gameSpace);
  if (gRuntime.mStageOffset) {
    std::printf("The game submits in STAGE, offset from LOCAL\n");
  }

  std::array<XrCompositionLayerProjectionView, 2> gameViews {};
  for (int eye = 0; eye < 2; ++eye) {
    RigidPose eyePose = HMDPose;
    eyePose.mPosition += Vector3::Transform(
      Vector3 {(eye == 0 ? -IPD : IPD) / 2, 0, 0}, HMDPose.mOrientation);
    gameViews[eye] = {
      .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
      // In the game's space
      .pose = ToXr(Relative(eyePose, SpacePose(gameSpace))),
      .fov = {-0.9f, 0.9f, 0.9f, -0.9f},
      .subImage = {
        .swapchain = ToHandle<XrSwapchain>(0xDEAD),
        .imageRect = {{0, 0}, {2000, 2000}},
      },
    };
  }
  const XrCompositionLayerProjection gameLayer {
    .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
    .space = gameSpace,
    .viewCount = 2,
    .views = gameViews.data(),
  };
  const auto gameLayerHeader
    = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&gameLayer);

  // '--recenter' (M5c, ADR-037)
  if (recenter) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto displayTime = [&] {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() + 1'000'000'000;
    };
    // Frames for `seconds` (the game polls events first, as games do)
    const auto run = [&](double seconds) {
      const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
      while (Clock::now() < until) {
        XrEventDataBuffer event {XR_TYPE_EVENT_DATA_BUFFER};
        while (layerPollEvent && layerPollEvent(instance, &event) == XR_SUCCESS) {
          event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        const XrFrameEndInfo frameEndInfo {
          .type = XR_TYPE_FRAME_END_INFO,
          .displayTime = displayTime(),
          .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
          .layerCount = 1,
          .layers = &gameLayerHeader,
        };
        layerEndFrame(session, &frameEndInfo);
        std::this_thread::sleep_for(std::chrono::microseconds(11111));
      }
    };
    // Where each panel is (by its swapchain), in the world
    using Placement = std::map<uint64_t, RigidPose>;
    const auto placements = [] {
      Placement placed;
      for (const auto& quad: gRuntime.mQuads) {
        placed[FromHandle(quad.subImage.swapchain)] = QuadWorldPose(quad);
      }
      return placed;
    };
    // The head, level: what panels are placed from after a recenter
    const auto level = [](const RigidPose& head) {
      const auto forward = Vector3::Transform(-Vector3::UnitZ, head.mOrientation);
      const float yaw = std::atan2(-forward.x, -forward.z);
      return RigidPose {head.mPosition, Quaternion::CreateFromAxisAngle(Vector3::UnitY, yaw)};
    };
    // The largest difference between two placements: metres, and degrees
    const auto differ = [](const Placement& a, const Placement& b) {
      float metres = a.size() == b.size() && !a.empty() ? 0.0f : 99.0f;
      float degrees = 0;
      for (const auto& [id, pose]: a) {
        const auto other = b.find(id);
        if (other == b.end()) {
          return std::pair {99.0f, 180.0f};
        }
        metres = std::max(metres, Vector3::Distance(pose.mPosition, other->second.mPosition));
        const float dot = std::abs(pose.mOrientation.Dot(other->second.mOrientation));
        degrees = std::max(degrees, 2.0f * std::acos(std::min(dot, 1.0f)) * 180.0f / 3.14159265f);
      }
      return std::pair {metres, degrees};
    };
    // Each panel placed from `origin`, as it was from LOCAL's origin at the
    // start
    const auto expected = [](const Placement& atStart, const RigidPose& origin) {
      Placement placed;
      for (const auto& [id, pose]: atStart) {
        placed[id] = ComposePose(origin, pose);
      }
      return placed;
    };

    std::printf("Recenter (test round 13): waiting for the host's panels\n");
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (gRuntime.mQuads.size() < 2 && Clock::now() < deadline) {
      run(0.1);
    }
    run(1.0);
    const auto atStart = placements();
    checks.Check(atStart.size() >= 2, std::format("the host's test panels show ({})", atStart.size()));

    // The head moves (stands up a little, leans, turns): nothing moves yet
    const auto headAtStart = HMDPose;
    HMDPose = {headAtStart.mPosition + Vector3 {0.15f, 0.25f, 0.10f},
      ComposePose({{}, headAtStart.mOrientation}, {{}, Quaternion::CreateFromAxisAngle(Vector3::UnitY, 0.45f)}).mOrientation};
    run(0.5);
    auto [moved, turned] = differ(placements(), atStart);
    checks.Check(moved < 0.001f && turned < 0.1f,
      std::format("moving the head moves no panel ({:.1f} mm, {:.2f} degrees)", moved * 1000, turned));

    // The runtime recenters as VDXR did in round 13: LOCAL's new origin
    // 45 cm above the head, turned with it; the event for LOCAL and STAGE
    const auto head = HMDPose;
    gRuntime.mLocal = {head.mPosition + Vector3 {0, 0.45f, 0}, level(head).mOrientation};
    for (const auto type: {XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE}) {
      XrEventDataBuffer buffer {XR_TYPE_EVENT_DATA_BUFFER};
      auto& changed = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&buffer);
      changed = {XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING, nullptr, session, type, displayTime(), XR_FALSE, {}};
      gRuntime.mEvents.push_back(buffer);
    }
    run(1.0);
    std::tie(moved, turned) = differ(placements(), expected(atStart, level(head)));
    checks.Check(moved < 0.005f && turned < 0.5f,
      std::format("after the runtime's recenter, panels sit from the head as they sat from LOCAL's origin, not 45 cm "
                  "up with LOCAL's new origin ({:.1f} mm, {:.2f} degrees off)",
        moved * 1000, turned));

    // The host's "Recenter" keybind (--test-recenter-after), with the head
    // somewhere else again
    HMDPose = {head.mPosition + Vector3 {-0.20f, -0.10f, 0.05f},
      ComposePose({{}, head.mOrientation}, {{}, Quaternion::CreateFromAxisAngle(Vector3::UnitY, -0.7f)}).mOrientation};
    const auto keybindHead = HMDPose;
    const auto keybindDeadline = Clock::now() + std::chrono::seconds(40);
    bool followed = false;
    while (!followed && Clock::now() < keybindDeadline) {
      run(0.2);
      std::tie(moved, turned) = differ(placements(), expected(atStart, level(keybindHead)));
      followed = moved < 0.005f && turned < 0.5f;
    }
    checks.Check(followed,
      std::format("the host's Recenter keybind puts panels the same way, where the head is then ({:.1f} mm, {:.2f} degrees off)",
        moved * 1000, turned));

    layerDestroySession(session);
    layerDestroyInstance(instance);
    std::printf("\n%s (%d failed)\n", checks.mFailures == 0 ? "PASSED" : "FAILED", checks.mFailures);
    return checks.mFailures == 0 ? 0 : 1;
  }

  // M6d (ADR-041): the quick menu, tapped, end to end. The host was started
  // with --show-quick-menu (open from the start, over its two test panels).
  // Each poke hovers 5 cm out, goes 8 mm through, holds and pulls back, as
  // on the keyboard; where to poke comes from the menu's own layout.
  if (quickMenu) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto displayTime = [&] {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() + 1'000'000'000;
    };
    const auto frame = [&](const std::optional<HandJoints>& hand) {
      XrEventDataBuffer event {XR_TYPE_EVENT_DATA_BUFFER};
      while (layerPollEvent && layerPollEvent(instance, &event) == XR_SUCCESS) {
        event = {XR_TYPE_EVENT_DATA_BUFFER};
      }
      gRuntime.mHands = {};
      if (hand) {
        gRuntime.mHands[1] = *hand;
      }
      const XrFrameEndInfo frameEndInfo {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = displayTime(),
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 1,
        .layers = &gameLayerHeader,
      };
      layerEndFrame(session, &frameEndInfo);
      std::this_thread::sleep_for(std::chrono::microseconds(11111));
    };
    // Frames with no hand until `done`, or `seconds` pass; true if done
    const auto until = [&](double seconds, const std::function<bool()>& done) {
      const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
      while (Clock::now() < deadline) {
        frame(std::nullopt);
        if (done()) {
          return true;
        }
      }
      return false;
    };
    // The menu's layer: open (256 mm wide) or closed (its 40 mm button)
    const float openWidth = static_cast<float>(QuickMenu::Width) * QuickMenu::MetersPerPixel;
    const float closedWidth = static_cast<float>(QuickMenu::ButtonSize) * QuickMenu::MetersPerPixel;
    const auto findMenu = [](float width) -> std::optional<XrCompositionLayerQuad> {
      for (const auto& quad: gRuntime.mQuads) {
        if (std::abs(quad.size.width - width) < 0.003f) {
          return quad;
        }
      }
      return std::nullopt;
    };
    // A poke at (u, v) on `quad`, over a second, then the hand goes
    const auto poke = [&](const XrCompositionLayerQuad& quad, float u, float v) {
      const auto begin = Clock::now();
      while (true) {
        const double phase = std::chrono::duration<double>(Clock::now() - begin).count();
        if (phase >= 1.0) {
          break;
        }
        float distance = 0.05f;
        if (phase >= 0.45 && phase < 0.6) {
          distance = 0.05f + ((-0.008f - 0.05f) * static_cast<float>((phase - 0.45) / 0.15));
        } else if (phase >= 0.6 && phase < 0.7) {
          distance = -0.008f;
        } else if (phase >= 0.7) {
          distance = -0.008f + ((0.05f + 0.008f) * static_cast<float>((phase - 0.7) / 0.3));
        }
        frame(HandAt(Pose::Point, quad, u, v, distance));
      }
      for (int i = 0; i < 5; ++i) {
        frame(std::nullopt);
      }
    };

    // The start page as the host lays it out for two panels
    QuickMenu::MenuData data;
    data.mPanels.resize(2);
    data.mPanels[0].mID = 1;
    data.mPanels[1].mID = 2;
    data.mProfiles = {{"test", L"Test panels", true}};
    data.mProfileName = L"Test panels";
    data.mAnyShowing = true;
    const auto layout = QuickMenu::BuildLayout({.mOpen = true}, data);
    const auto middleOf = [&](auto&& match) -> std::pair<float, float> {
      for (const auto& item: layout.mItems) {
        if (match(item)) {
          return {((item.mRect.mLeft + item.mRect.mRight) / 2) / static_cast<float>(layout.mWidth),
            ((item.mRect.mTop + item.mRect.mBottom) / 2) / static_cast<float>(layout.mHeight)};
        }
      }
      return {0.5f, 0.5f};
    };
    const auto firstSwitch = middleOf([](const QuickMenu::Item& item) { return item.mKind == QuickMenu::ItemKind::Toggle; });
    const auto close = middleOf([](const QuickMenu::Item& item) { return item.mCommand.mKind == QuickMenu::CommandKind::Close; });

    std::optional<XrCompositionLayerQuad> menu;
    const bool shown = until(20, [&] { return (menu = findMenu(openWidth)).has_value(); });
    checks.Check(shown, "the quick menu shows, open, 256 mm wide");
    if (!shown) {
      layerDestroySession(session);
      layerDestroyInstance(instance);
      std::printf("\nFAILED (%d failed)\n", checks.mFailures);
      return 1;
    }
    until(1.0, [] { return false; });// its first frames settle
    menu = findMenu(openWidth);
    const int layers = gRuntime.mQuadCount;
    const float expectedHeight = static_cast<float>(layout.mHeight) * QuickMenu::MetersPerPixel;
    checks.Check(menu && std::abs(menu->size.height - expectedHeight) < 0.003f && layers == 3,
      std::format("laid out as the simulator expects: {:.0f} mm tall ({:.0f} expected), with the 2 test panels ({} layers)",
        menu ? menu->size.height * 1000 : 0.0f, expectedHeight * 1000, layers));
    SaveQuad(*menu, outputDir / "quick-menu.png");

    poke(*menu, firstSwitch.first, firstSwitch.second);
    checks.Check(until(2.0, [&] { return gRuntime.mQuadCount == layers - 1; }),
      std::format("tapping the first panel's switch hid that panel ({} layers)", gRuntime.mQuadCount));
    menu = findMenu(openWidth);
    if (menu) {
      SaveQuad(*menu, outputDir / "quick-menu-hidden.png");
      poke(*menu, firstSwitch.first, firstSwitch.second);
    }
    checks.Check(until(2.0, [&] { return gRuntime.mQuadCount == layers; }),
      std::format("tapping it again showed it ({} layers)", gRuntime.mQuadCount));

    menu = findMenu(openWidth);
    if (menu) {
      poke(*menu, close.first, close.second);
    }
    std::optional<XrCompositionLayerQuad> button;
    checks.Check(until(2.0, [&] { return (button = findMenu(closedWidth)).has_value() && !findMenu(openWidth); }),
      "the close button closed it, leaving its 40 mm button");
    if (button) {
      SaveQuad(*button, outputDir / "quick-menu-button.png");
      poke(*button, 0.5f, 0.5f);
    }
    checks.Check(until(2.0, [&] { return findMenu(openWidth).has_value(); }), "tapping the button opened it again");

    layerDestroySession(session);
    layerDestroyInstance(instance);
    std::printf("\n%s (%d failed)\n", checks.mFailures == 0 ? "PASSED" : "FAILED", checks.mFailures);
    return checks.mFailures == 0 ? 0 : 1;
  }

  // Typing on the virtual keyboard
  if (!typeKeys.empty()) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::optional<Clock::time_point> typingStart;
    // Per key: hover 5cm in front, poke 8mm through, hold, pull back
    constexpr double perKey = 0.75;
    // '--type': first, a tap on the window panel near its top (where keys
    // go: doc 07 "Target selection"), over this long
    const double tapSeconds = typeLog ? 2.0 : 0.0;
    bool tapped = false;
    bool savedPressed = false;
    int keyboardFrames = 0;
    int handLayerFrames = 0;
    std::optional<XrCompositionLayerQuad> keyboard;
    while (true) {
      const auto now = Clock::now();
      const double sinceStart
        = std::chrono::duration<double>(now - start).count();
      if (gRuntime.mKeyboardQuad) {
        keyboard = gRuntime.mKeyboardQuad;
        ++keyboardFrames;
        // '--type' taps the window panel first: it must be showing too
        if (!typingStart && (!typeLog || gRuntime.mPanelQuad)) {
          typingStart = now;
          std::printf(
            "Keyboard layer: %.2fm x %.2fm; pressing %zu keys\n",
            keyboard->size.width,
            keyboard->size.height,
            typeKeys.size());
        }
      }
      if (!typingStart && sinceStart > 15) {
        std::printf(
          "FAIL: no keyboard layer after 15 seconds; tap a text field in a "
          "captured window first\n");
        return 1;
      }
      gRuntime.mHands = {};
      const double sinceKeyboard
        = typingStart ? std::chrono::duration<double>(now - *typingStart).count() : 0.0;
      if (typingStart && sinceKeyboard < tapSeconds) {
        if (gRuntime.mPanelQuad) {
          // Hover 8 cm out, poke 1.5 cm through, hold, pull back
          const double phase = sinceKeyboard / tapSeconds;
          const auto at = [](double from, double to, double a, double b, double p) {
            return static_cast<float>(from + ((to - from) * std::clamp((p - a) / (b - a), 0.0, 1.0)));
          };
          float distance = 0.08f;
          if (phase >= 0.4 && phase < 0.55) {
            distance = at(0.08, -0.015, 0.4, 0.55, phase);
          } else if (phase >= 0.55 && phase < 0.65) {
            distance = -0.015f;
          } else if (phase >= 0.65 && phase < 0.85) {
            distance = at(-0.015, 0.08, 0.65, 0.85, phase);
          }
          if (!tapped && phase >= 0.55) {
            tapped = true;
            std::printf("Tapped the window panel: keys go to its window\n");
          }
          gRuntime.mHands[1] = HandAt(Pose::Point, *gRuntime.mPanelQuad, 0.5f, 0.08f, distance);
        }
      } else if (typingStart && keyboard) {
        const double t = sinceKeyboard - tapSeconds;
        const auto index = static_cast<size_t>(t / perKey);
        if (index >= typeKeys.size()) {
          if (t > (typeKeys.size() * perKey) + 0.5) {
            break;
          }
        } else {
          const double phase = (t - (index * perKey)) / perKey;
          const auto [u, v] = typeKeys.at(index);
          float distance = 0.05f;
          if (phase < 0.45) {
            distance = 0.05f;
          } else if (phase < 0.6) {
            distance = 0.05f
              + ((-0.008f - 0.05f) * static_cast<float>((phase - 0.45) / 0.15));
          } else if (phase < 0.7) {
            distance = -0.008f;
          } else {
            distance = -0.008f
              + ((0.05f + 0.008f) * static_cast<float>((phase - 0.7) / 0.3));
          }
          gRuntime.mHands[1] = HandAt(Pose::Point, *keyboard, u, v, distance);
        }
      }
      const XrFrameEndInfo frameEndInfo {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime
        = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start)
            .count()
          + 1'000'000'000,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 1,
        .layers = &gameLayerHeader,
      };
      layerEndFrame(session, &frameEndInfo);
      if (gRuntime.mHandLayerThisFrame) {
        ++handLayerFrames;
      }
      // The keyboard while the first key is held, for looking at
      if (typeLog && typingStart && keyboard && !savedPressed && sinceKeyboard > tapSeconds + (0.68 * perKey)) {
        savedPressed = true;
        SaveQuad(*keyboard, outputDir / "keyboard-pressed.png");
      }
      std::this_thread::sleep_until(now + std::chrono::microseconds(11111));
    }
    if (typeLog && keyboard) {
      SaveQuad(*keyboard, outputDir / "keyboard.png");
    }
    std::printf(
      "Typed; the keyboard was shown in %d frames, and hands were drawn in "
      "%d\n",
      keyboardFrames,
      handLayerFrames);
    std::printf(
      "Panel layers back to front: %d of %d frames with several layers "
      "were out of order\n",
      gRuntime.mFramesOutOfOrder,
      gRuntime.mFramesWithMultipleQuads);
    layerDestroySession(session);
    layerDestroyInstance(instance);
    if (!typeLog) {
      return 0;
    }
    // What the app has, once the last key has had time to arrive
    std::string typed;
    for (int i = 0; i < 30 && typed != typeText; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::ifstream file {*typeLog, std::ios::binary};
      typed.assign(std::istreambuf_iterator<char> {file}, std::istreambuf_iterator<char> {});
    }
    if (typed != typeText) {
      std::printf("FAIL: the app has \"%s\", not \"%s\"\n", typed.c_str(), typeText.c_str());
      return 1;
    }
    if (handLayerFrames == 0) {
      std::printf("FAIL: no hand drawn while typing\n");
      return 1;
    }
    std::printf("PASSED: the app received \"%s\" (doc 11, \"Keyboard: poke h, i\")\n", typed.c_str());
    return 0;
  }

  if (benchSeconds) {
    gRuntime.mBench = true;
    std::printf(
      "Benchmarking the layer for %.0fs%s\n",
      *benchSeconds,
      benchHover ? ", with a pointing hand hovering" : "");
    auto device = gRuntime.mDevice.get();
    auto context = gRuntime.mContext.get();
    constexpr size_t Ring = 8;
    struct Queries {
      winrt::com_ptr<ID3D11Query> mDisjoint;
      winrt::com_ptr<ID3D11Query> mBegin;
      winrt::com_ptr<ID3D11Query> mEnd;
      bool mPending {false};
    };
    std::array<Queries, Ring> queries;
    for (auto& q: queries) {
      D3D11_QUERY_DESC desc {D3D11_QUERY_TIMESTAMP_DISJOINT};
      winrt::check_hresult(device->CreateQuery(&desc, q.mDisjoint.put()));
      desc.Query = D3D11_QUERY_TIMESTAMP;
      winrt::check_hresult(device->CreateQuery(&desc, q.mBegin.put()));
      winrt::check_hresult(device->CreateQuery(&desc, q.mEnd.put()));
    }
    std::vector<double> gpuMs;
    const auto collect = [&](Queries& q) {
      if (!q.mPending) {
        return;
      }
      q.mPending = false;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
      while (context->GetData(q.mDisjoint.get(), &disjoint, sizeof(disjoint), 0)
             == S_FALSE) {
        std::this_thread::yield();
      }
      uint64_t begin {}, end {};
      while (context->GetData(q.mBegin.get(), &begin, sizeof(begin), 0)
             == S_FALSE) {
        std::this_thread::yield();
      }
      while (context->GetData(q.mEnd.get(), &end, sizeof(end), 0) == S_FALSE) {
        std::this_thread::yield();
      }
      if (disjoint.Disjoint || disjoint.Frequency == 0 || end < begin) {
        return;
      }
      gpuMs.push_back(
        static_cast<double>(end - begin) * 1000.0 / disjoint.Frequency);
    };

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::optional<Clock::time_point> measureStart;
    uint64_t frames = 0;
    uint64_t releasesAtStart = 0;
    uint64_t pixelsAtStart = 0;
    size_t frameIndex = 0;
    while (true) {
      const auto now = Clock::now();
      const double t = std::chrono::duration<double>(now - start).count();
      // A few seconds to settle (swapchains created, host app connected)
      if (!measureStart && t > 3 && gRuntime.mQuad) {
        measureStart = now;
        releasesAtStart = gRuntime.mReleases;
        pixelsAtStart = gRuntime.mReleasedPixels;
        gRuntime.mBenchHandFrames = 0;
        gRuntime.mBenchHandCoverage = 0;
        gpuMs.clear();
        frames = 0;
      }
      if (
        measureStart
        && (now - *measureStart)
          > std::chrono::duration<double>(*benchSeconds)) {
        break;
      }
      if (t > 20 && !gRuntime.mQuad) {
        std::printf(
          "FAIL: no panel after 20 seconds; is the host app running, with a "
          "panel enabled in VR?\n");
        return 1;
      }
      gRuntime.mHands = {};
      if (benchHover && gRuntime.mQuad) {
        gRuntime.mHands[1]
          = HandAt(Pose::Point, *gRuntime.mQuad, 0.5f, 0.55f, 0.08f);
      }
      const XrFrameEndInfo frameEndInfo {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime
        = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start)
            .count()
          + 1'000'000'000,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 1,
        .layers = &gameLayerHeader,
      };
      auto& q = queries[frameIndex % Ring];
      collect(q);
      context->Begin(q.mDisjoint.get());
      context->End(q.mBegin.get());
      layerEndFrame(session, &frameEndInfo);
      context->End(q.mEnd.get());
      context->End(q.mDisjoint.get());
      q.mPending = true;
      context->Flush();
      ++frameIndex;
      if (measureStart) {
        ++frames;
      }
      std::this_thread::sleep_until(now + std::chrono::microseconds(11111));
    }
    for (auto& q: queries) {
      collect(q);
    }
    std::ranges::sort(gpuMs);
    double sum = 0;
    for (const auto ms: gpuMs) {
      sum += ms;
    }
    const auto seconds = *benchSeconds;
    std::printf(
      "Layer GPU time per frame: avg %.3fms, median %.3fms, p95 %.3fms (%zu "
      "frames)\n",
      gpuMs.empty() ? 0.0 : sum / gpuMs.size(),
      gpuMs.empty() ? 0.0 : gpuMs[gpuMs.size() / 2],
      gpuMs.empty() ? 0.0 : gpuMs[(gpuMs.size() * 95) / 100],
      gpuMs.size());
    std::printf(
      "Swapchain images released: %.1f/s (%.1f Mpx/s); %llu frames\n",
      (gRuntime.mReleases - releasesAtStart) / seconds,
      (gRuntime.mReleasedPixels - pixelsAtStart) / seconds / 1e6,
      static_cast<unsigned long long>(frames));
    std::printf(
      "Hand layer: in %d of %llu frames, covering %.1f%% of each eye's view "
      "on average when shown\n",
      gRuntime.mBenchHandFrames,
      static_cast<unsigned long long>(frames),
      gRuntime.mBenchHandFrames
        ? 100.0 * gRuntime.mBenchHandCoverage / gRuntime.mBenchHandFrames
        : 0.0);
    layerDestroySession(session);
    layerDestroyInstance(instance);
    return 0;
  }

  if (watchSeconds) {
    std::printf("Watching the panel layers for %.0fs\n", *watchSeconds);
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    struct Seen {
      int mCount {-1};
      float mWidth {};
      float mBrightness {-1};
    } last;
    while (true) {
      const auto now = Clock::now();
      const double t = std::chrono::duration<double>(now - start).count();
      if (t > *watchSeconds) {
        break;
      }
      const XrFrameEndInfo frameEndInfo {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime
        = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start)
            .count()
          + 1'000'000'000,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 1,
        .layers = &gameLayerHeader,
      };
      layerEndFrame(session, &frameEndInfo);

      Seen seen {.mCount = gRuntime.mQuadCount};
      if (seen.mCount > 0 && gRuntime.mQuad) {
        seen.mWidth = gRuntime.mQuad->size.width;
        seen.mBrightness = PanelBrightness(*gRuntime.mQuad).value_or(-1);
      }
      const bool changed = seen.mCount != last.mCount
        || std::abs(seen.mWidth - last.mWidth) > 0.01f
        || std::abs(seen.mBrightness - last.mBrightness) > 6;
      if (changed) {
        if (seen.mCount > 0) {
          std::printf(
            "  %5.2fs: %d panel layer(s), %.2fm wide, brightness %.0f\n",
            t,
            seen.mCount,
            seen.mWidth,
            seen.mBrightness);
        } else {
          std::printf("  %5.2fs: no panel layers\n", t);
        }
        last = seen;
      }
      std::this_thread::sleep_until(now + std::chrono::microseconds(11111));
    }
    layerDestroySession(session);
    layerDestroyInstance(instance);
    return 0;
  }

  if (gestures) {
    if (!clickLog) {
      std::printf("FAIL: --gestures needs click-target's log\n");
      return 1;
    }
    const auto steps = GestureSteps();
    std::vector<double> stepStarts(steps.size(), 0);
    std::printf(
      "Running the M4c gestures; the host app must show click-target in a "
      "panel\n");
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::optional<Clock::time_point> scriptStart;
    std::size_t current = steps.size();
    std::optional<XrCompositionLayerQuad> panel;
    // The panel first shows at a provisional size while the window's first
    // frame arrives: start once its size has held for half a second
    std::optional<Clock::time_point> sizeSince;
    XrExtent2Df lastSize {};
    while (true) {
      const auto now = Clock::now();
      if (gRuntime.mQuad && !scriptStart) {
        const auto size = gRuntime.mQuad->size;
        if (!sizeSince || size.width != lastSize.width || size.height != lastSize.height) {
          sizeSince = now;
          lastSize = size;
        } else if (now - *sizeSince > std::chrono::milliseconds(500)) {
          scriptStart = now;
          std::printf(
            "Panel %.3fm x %.3fm; click-target is 800 x 600\n",
            size.width,
            size.height);
        }
      }
      if (gRuntime.mQuad) {
        panel = gRuntime.mQuad;
      }
      if (
        !scriptStart
        && std::chrono::duration<double>(now - start).count() > 15) {
        std::printf(
          "FAIL: no settled panel after 15 seconds; is the host app showing "
          "click-target?\n");
        return 1;
      }
      gRuntime.mHands = {};
      if (scriptStart) {
        const double t = std::chrono::duration<double>(now - *scriptStart).count();
        std::size_t index = 0;
        double stepStart = 0;
        while (index < steps.size() && t >= stepStart + steps[index].mSeconds) {
          stepStart += steps[index].mSeconds;
          ++index;
        }
        if (index >= steps.size()) {
          break;
        }
        const auto& step = steps[index];
        if (index != current) {
          current = index;
          stepStarts[index] = NowMilliseconds();
          std::printf("  ... %s\n", step.mName);
          if (step.mFailHandWait) {
            gRuntime.mFailNextHandWait = true;
          }
        }
        gRuntime.mCheckDrawnWhereItIs = step.mCheckDrawn;
        gRuntime.mHands[1] = step.mHand(
          static_cast<float>((t - stepStart) / step.mSeconds), *panel);
      }
      const XrFrameEndInfo frameEndInfo {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime
        = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()
          + 1'000'000'000,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 1,
        .layers = &gameLayerHeader,
      };
      layerEndFrame(session, &frameEndInfo);
      std::this_thread::sleep_until(now + std::chrono::microseconds(11111));
    }
    gRuntime.mCheckDrawnWhereItIs = false;
    // Give the app time to deliver the last events
    std::this_thread::sleep_for(std::chrono::seconds(1));
    layerDestroySession(session);
    layerDestroyInstance(instance);

    // Each scenario's clicks: from its first step to the next scenario's
    const auto lines = ReadLines(*clickLog);
    const auto clicks = TimedClicks(lines);
    const auto startOf = [&](Scenario scenario) {
      for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].mScenario == scenario) {
          return stepStarts[i];
        }
      }
      return 1e300;
    };
    const auto stepStartOf = [&](Scenario scenario, std::string_view name) {
      for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].mScenario == scenario && std::string_view {steps[i].mName}.contains(name)) {
          return stepStarts[i];
        }
      }
      return 1e300;
    };
    const auto during = [&](Scenario scenario, std::string_view kind) {
      const double from = startOf(scenario);
      const double to = startOf(static_cast<Scenario>(static_cast<int>(scenario) + 1));
      std::vector<Click> found;
      for (const auto& click: clicks) {
        if (click.mKind == kind && click.mAt >= from && click.mAt < to) {
          found.push_back(click);
        }
      }
      return found;
    };
    const float metersPerPixel = panel->size.width / 800.0f;

    std::printf("\nResults\n");
    {
      const auto downs = during(Scenario::AngledPoke, "DOWN");
      const auto ups = during(Scenario::AngledPoke, "UP");
      checks.Check(
        downs.size() == 1 && ups.size() == 1,
        std::format("angled poke: one click ({} down, {} up)", downs.size(), ups.size()));
      if (!downs.empty()) {
        const double aimedX = AngledU * 800;
        const double aimedY = AngledV * 600;
        const double missMm
          = std::hypot(downs[0].mX - aimedX, downs[0].mY - aimedY) * metersPerPixel * 1000;
        checks.Check(
          missMm <= 1.0,
          std::format(
            "angled poke: the click is {:.2f} mm from where the finger met the "
            "panel, at ({}, {}) for ({}, {}) (gate: 1 mm)",
            missMm, downs[0].mX, downs[0].mY, aimedX, aimedY));
      }
    }
    {
      const auto downs = during(Scenario::HoldDrag, "DOWN");
      const auto ups = during(Scenario::HoldDrag, "UP");
      checks.Check(
        downs.size() == 1 && ups.size() == 1,
        std::format("hold 1 s then drag 40 mm: one down, one up ({} down, {} up)", downs.size(), ups.size()));
      if (downs.size() == 1 && ups.size() == 1) {
        const double dragEnded = stepStartOf(Scenario::HoldDrag, "holding at the end");
        checks.Check(
          ups[0].mAt >= dragEnded,
          std::format(
            "hold and drag: no early release (up {:.0f} ms after the drag ended)",
            ups[0].mAt - dragEnded));
        const double draggedMm = (ups[0].mX - downs[0].mX) * metersPerPixel * 1000;
        checks.Check(
          std::abs(draggedMm - 40) <= 4 && std::abs(ups[0].mY - downs[0].mY) * metersPerPixel * 1000 <= 2,
          std::format("hold and drag: let go {:.1f} mm right of the press (40 mm, within 4)", draggedMm));
        // click-target writes the moves since the last button before each
        // one: those before this release came while it was held
        int dragMoves = 0;
        for (std::size_t i = 1; i < lines.size(); ++i) {
          const auto release = TimedClicks({lines[i]});
          if (
            !release.empty() && release[0].mKind == "UP"
            && std::abs(release[0].mAt - ups[0].mAt) < 0.01
            && lines[i - 1].starts_with("MOVES ")) {
            dragMoves = std::atoi(lines[i - 1].c_str() + 6);
          }
        }
        checks.Check(dragMoves >= 5, std::format("hold and drag: moves while held ({})", dragMoves));
      }
    }
    {
      int wheelTotal = 0;
      for (const auto& line: lines) {
        if (line.starts_with("WHEEL ")) {
          wheelTotal += std::atoi(line.c_str() + 6);
        }
      }
      checks.Check(
        wheelTotal == 360,
        std::format(
          "scroll: two fingers down 36.5 mm turned the wheel 3 notches up, the "
          "natural way ({} notches)",
          wheelTotal / 120.0));
      checks.Check(
        during(Scenario::Scroll, "DOWN").empty(),
        "scroll: no click");
    }
    {
      const auto downs = during(Scenario::Gap300, "DOWN");
      const auto ups = during(Scenario::Gap300, "UP");
      const double lostAt = stepStartOf(Scenario::Gap300, "tracking lost");
      const double backAt = stepStartOf(Scenario::Gap300, "back, still");
      checks.Check(
        downs.size() == 1 && ups.size() == 1,
        std::format("300 ms tracking gap during a drag: released once, no click on return ({} down, {} up)", downs.size(), ups.size()));
      if (ups.size() == 1) {
        checks.Check(
          ups[0].mAt > lostAt + 200 && ups[0].mAt < backAt + 50,
          std::format("300 ms gap: released {:.0f} ms into it (after 250 ms)", ups[0].mAt - lostAt));
      }
    }
    {
      const auto downs = during(Scenario::Gap100, "DOWN");
      const auto ups = during(Scenario::Gap100, "UP");
      const double pulledAt = stepStartOf(Scenario::Gap100, "pulling back");
      checks.Check(
        downs.size() == 1 && ups.size() == 1 && ups[0].mAt >= pulledAt,
        std::format(
          "100 ms tracking gap during a drag: the drag continued, released only "
          "when pulled back ({} down, {} up)",
          downs.size(), ups.size()));
    }
    {
      checks.Check(
        during(Scenario::ReachBehind, "DOWN").empty() && during(Scenario::ReachBehind, "UP").empty(),
        "reach behind the panel: no clicks");
      checks.Check(
        gRuntime.mDrawnChecks > 5 && gRuntime.mDrawnWorstOffset <= 15
          && gRuntime.mDrawnNearestLimited > 2 * gRuntime.mDrawnWorstOffset,
        std::format(
          "reach behind: drawn where the hand is, not pushed to the panel's "
          "surface ({} frames: at most {:.1f} px from the hand; touch "
          "limiting would put it {:.0f} px away)",
          gRuntime.mDrawnChecks,
          gRuntime.mDrawnWorstOffset,
          gRuntime.mDrawnNearestLimited));
    }
    checks.Check(
      gRuntime.mHandWaitFailures == 1 && gRuntime.mHandSwapchainsDestroyed >= 1
        && gRuntime.mNewHandSwapchainAfterFailure && gRuntime.mHandLayerFramesAfterFailure > 10,
      std::format(
        "the hand layer's image wait failed once: its swapchain was re-created "
        "and drawing resumed ({} failed, {} destroyed, {} hand layer frames "
        "after, on a new swapchain: {})",
        gRuntime.mHandWaitFailures,
        gRuntime.mHandSwapchainsDestroyed,
        gRuntime.mHandLayerFramesAfterFailure,
        gRuntime.mNewHandSwapchainAfterFailure ? "yes" : "no"));
    checks.Check(
      gRuntime.mHandLayerViewMismatches == 0,
      std::format(
        "the hand layer always used the game's own views ({} frames didn't)",
        gRuntime.mHandLayerViewMismatches));
    checks.Check(CountPrefix(lines, "DOUBLE") == 0, "no double-clicks");
    std::printf(
      "\n%s (%d failed)\n",
      checks.mFailures == 0 ? "PASSED" : "FAILED",
      checks.mFailures);
    return checks.mFailures == 0 ? 0 : 1;
  }

  std::printf(
    "Running the script; the host app must be running with a panel in VR\n");
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  std::optional<Clock::time_point> scriptStart;
  std::map<Step, Stats> stats;
  int scriptFrames = 0;
  std::optional<double> scriptFirstFrameAt;
  double scriptLastFrameAt = 0;
  // When the frame after the one that touched started: the latency gate's
  // "one frame" is that frame as it really ran (M5c: a Debug simulator's
  // frames are one or two 15.6 ms timer ticks, so its average was the
  // wrong measure for the one frame that counts)
  std::optional<double> afterContactFrameAt;
  Step lastStep = Step::Waiting;
  bool savedTouching = false;
  bool savedHovering = false;
  int maxCoverageWhilePointing = 0;

  while (true) {
    const auto now = Clock::now();
    const double sinceStart
      = std::chrono::duration<double>(now - start).count();
    if (gRuntime.mQuad && !scriptStart) {
      scriptStart = now;
    }
    if (!scriptStart && sinceStart > 15) {
      std::printf(
        "FAIL: no panel after 15 seconds; is the host app running, with "
        "a panel enabled in VR?\n");
      return 1;
    }

    Step step = Step::Waiting;
    double progress = 0;
    if (scriptStart) {
      const double t = std::chrono::duration<double>(now - *scriptStart).count();
      if (t >= Script.back().mEnd) {
        break;
      }
      for (const auto& entry: Script) {
        if (t >= entry.mStart && t < entry.mEnd) {
          step = entry.mStep;
          progress = (t - entry.mStart) / (entry.mEnd - entry.mStart);
        }
      }
      ScriptHands(step, progress, *gRuntime.mQuad);
    }
    if (step != lastStep) {
      std::printf("  ... %s\n", StepName(step));
      lastStep = step;
    }

    // The press latency gate (doc 11) starts at the frame whose tracking
    // sample reports contact
    if (gRuntime.mContactAt && !afterContactFrameAt) {
      afterContactFrameAt = NowMilliseconds();
    }
    if (step == Step::PokeIn && !gRuntime.mContactAt && 0.08 + (-0.015 - 0.08) * progress <= 0.010) {
      gRuntime.mContactAt = NowMilliseconds();
    }
    if (step == Step::PokeIn && !gRuntime.mWatchPanel) {
      gRuntime.mWatchU = TargetU;
      gRuntime.mWatchV = TargetV;
      gRuntime.mWatchPanel = true;
    }
    gRuntime.mCheckHandPlacement = step == Step::Pointing || step == Step::Hovering;
    if (step == Step::Touching && progress > 0.5 && !savedTouching) {
      gRuntime.mSaveHandLayerTo = outputDir / "sim-hand-touching.png";
      savedTouching = true;
    }
    if (step == Step::Hovering && progress > 0.5 && !savedHovering) {
      gRuntime.mSaveHandLayerTo = outputDir / "sim-hand-hovering.png";
      savedHovering = true;
    }

    const XrFrameEndInfo frameEndInfo {
      .type = XR_TYPE_FRAME_END_INFO,
      .displayTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       now - start)
                       .count()
        + 1'000'000'000,
      .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
      .layerCount = 1,
      .layers = &gameLayerHeader,
    };
    layerEndFrame(session, &frameEndInfo);
    // The real frame interval: sleeps run at the system timer's resolution
    if (scriptStart) {
      ++scriptFrames;
      scriptLastFrameAt = NowMilliseconds();
      if (!scriptFirstFrameAt) {
        scriptFirstFrameAt = scriptLastFrameAt;
      }
    }

    auto& s = stats[step];
    ++s.mFrames;
    if (gRuntime.mHandLayerThisFrame) {
      ++s.mHandLayerFrames;
      if (step == Step::Pointing || step == Step::Hovering) {
        maxCoverageWhilePointing
          = std::max(maxCoverageWhilePointing, gRuntime.mHandLayerCoverage);
      }
    }
    std::this_thread::sleep_until(now + std::chrono::microseconds(11111));
  }

  std::printf("\nResults\n");
  checks.Check(
    gRuntime.mHandTrackersCreated == 2,
    std::format(
      "the layer started tracking both hands ({} trackers); if 0, is "
      "tapping panels with your index finger on in the host app's settings?",
      gRuntime.mHandTrackersCreated));
  checks.Check(
    stats[Step::FlatHand].mHandLayerFrames == 0,
    std::format(
      "a flat hand stays hidden ({} of {} frames showed a hand)",
      stats[Step::FlatHand].mHandLayerFrames,
      stats[Step::FlatHand].mFrames));
  const auto& pointing = stats[Step::Pointing];
  checks.Check(
    pointing.mHandLayerFrames > pointing.mFrames / 2,
    std::format(
      "a pointing hand is shown ({} of {} frames)",
      pointing.mHandLayerFrames,
      pointing.mFrames));
  checks.Check(
    maxCoverageWhilePointing > 500,
    std::format(
      "the hand layer has a visible hand in it ({} pixels drawn)",
      maxCoverageWhilePointing));
  checks.Check(
    stats[Step::Touching].mHandLayerFrames > 0,
    "the hand stays visible while touching");
  checks.Check(
    gRuntime.mHandPlacementChecks > 0 && gRuntime.mHandPlacementMisses == 0,
    std::format(
      "the drawn hand is where the hand is: its fingertip inside the drawn "
      "hand ({} of {} frames weren't)",
      gRuntime.mHandPlacementMisses,
      gRuntime.mHandPlacementChecks));
  checks.Check(
    gRuntime.mHandLayerViewMismatches == 0,
    std::format(
      "the hand layer always uses the game's own eye poses and field of view, "
      "over its whole image ({} frames didn't)",
      gRuntime.mHandLayerViewMismatches));
  if (depthOrder) {
    checks.Check(
      gRuntime.mFramesWithMultipleQuads > 100 && gRuntime.mFramesOutOfOrder == 0,
      std::format(
        "overlapping panels at different depths, given nearest first: back "
        "to front in every frame ({} of {} frames weren't)",
        gRuntime.mFramesOutOfOrder,
        gRuntime.mFramesWithMultipleQuads));
  }

  if (clickLog) {
    // Give the app time to deliver the last events
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto lines = ReadLines(*clickLog);
    const auto downs = CountPrefix(lines, "DOWN");
    const auto ups = CountPrefix(lines, "UP");
    checks.Check(
      downs == 1 && ups == 1,
      std::format(
        "the click target got exactly one click ({} down, {} up); the flat "
        "hand sweep added none",
        downs,
        ups));
    for (const auto& line: lines) {
      if (line.starts_with("DOWN")) {
        std::printf("  click target: %s (window is 800x600)\n", line.c_str());
      }
    }
    // At the aimed point (doc 11; in STAGE too: "taps still land on target")
    if (const auto clicks = TimedClicks(lines); !clicks.empty() && clicks[0].mKind == "DOWN" && gRuntime.mQuad) {
      const double missMm = std::hypot(clicks[0].mX - (TargetU * 800), clicks[0].mY - (TargetV * 600))
        * gRuntime.mQuad->size.width / 800 * 1000;
      checks.Check(
        missMm <= 2.0,
        std::format("the click landed {:.2f} mm from the point aimed at (within 2 mm)", missMm));
    }
    checks.Check(
      CountPrefix(lines, "MOVES") > 0, "the cursor moved over the window");

    int wheelMessages = 0;
    int wheelTotal = 0;
    for (const auto& line: lines) {
      if (line.starts_with("WHEEL ")) {
        ++wheelMessages;
        wheelTotal += std::atoi(line.c_str() + 6);
      }
    }
    checks.Check(
      wheelMessages > 0 && wheelTotal > 0,
      std::format(
        "dragging two fingers down scrolled the window up ({} wheel messages, "
        "{} notches), without clicking",
        wheelMessages,
        wheelTotal / 120));

    // From the click reaching the window, to the panel showing the
    // window's reaction (its orange click marker) in what the layer submits
    for (const auto& line: lines) {
      if (!line.starts_with("DOWN ")) {
        continue;
      }
      std::istringstream fields(line.substr(5));
      int x {}, y {};
      double clickedAt {};
      if (!(fields >> x >> y >> clickedAt)) {
        break;
      }
      if (gRuntime.mContactAt && scriptFirstFrameAt && scriptFrames > 1) {
        // One frame plus 2 ms (doc 11, "Latency"): the frame after contact,
        // as it really ran, or the average frame if that was shorter (a
        // press the depth filter catches a frame later is within the gate)
        const double average
          = (scriptLastFrameAt - *scriptFirstFrameAt) / (scriptFrames - 1);
        const double frame = std::max(average, afterContactFrameAt ? *afterContactFrameAt - *gRuntime.mContactAt : 0.0);
        const double latency = clickedAt - *gRuntime.mContactAt;
        checks.Check(
          latency <= frame + 2.0,
          std::format(
            "the press reached the window {:.1f}ms after the frame that "
            "touched (gate: 1 frame of {:.1f}ms + 2 ms)",
            latency,
            frame));
      }
      if (gRuntime.mPanelChangedAt) {
        std::printf(
          "  click -> window reacts -> visible in the panel layer: %.1fms\n",
          *gRuntime.mPanelChangedAt - clickedAt);
      } else {
        std::printf("  (the click marker wasn't seen on the panel)\n");
      }
      break;
    }
  }

  layerDestroySession(session);
  layerDestroyInstance(instance);

  std::printf(
    "\n%s (%d failed)\n",
    checks.mFailures == 0 ? "PASSED" : "FAILED",
    checks.mFailures);
  return checks.mFailures == 0 ? 0 : 1;
}
