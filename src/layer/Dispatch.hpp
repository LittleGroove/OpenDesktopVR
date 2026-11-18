// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The next layer's (or runtime's) functions for one instance. The list is in
// one place; add a function to the right macro and it is declared and loaded.
#pragma once

#include "XrPlatform.hpp"

namespace OpenDesktopVR::Layer {

// Functions every composition needs
#define ODVR_NEXT_CORE_FUNCTIONS(X) \
  X(xrDestroyInstance) \
  X(xrGetSystemProperties) \
  X(xrCreateSession) \
  X(xrDestroySession) \
  X(xrEndFrame) \
  X(xrCreateReferenceSpace) \
  X(xrDestroySpace) \
  X(xrLocateSpace) \
  X(xrEnumerateSwapchainFormats) \
  X(xrCreateSwapchain) \
  X(xrDestroySwapchain) \
  X(xrEnumerateSwapchainImages) \
  X(xrAcquireSwapchainImage) \
  X(xrWaitSwapchainImage) \
  X(xrReleaseSwapchainImage)

// Functions that are nice to have; missing ones are simply not used
#define ODVR_NEXT_OPTIONAL_FUNCTIONS(X) \
  X(xrGetInstanceProperties) \
  X(xrPollEvent)

// XR_EXT_hand_tracking, when enabled on the instance
#define ODVR_NEXT_HAND_FUNCTIONS(X) \
  X(xrCreateHandTrackerEXT) \
  X(xrDestroyHandTrackerEXT) \
  X(xrLocateHandJointsEXT)

struct NextFunctions {
  PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr {nullptr};
#define ODVR_DECLARE(name) PFN_##name name {nullptr};
  ODVR_NEXT_CORE_FUNCTIONS(ODVR_DECLARE)
  ODVR_NEXT_OPTIONAL_FUNCTIONS(ODVR_DECLARE)
  ODVR_NEXT_HAND_FUNCTIONS(ODVR_DECLARE)
#undef ODVR_DECLARE

  bool mHasCore {false};
  bool mHasHandTracking {false};

  void Load(XrInstance instance, PFN_xrGetInstanceProcAddr next, bool handTrackingEnabled) noexcept {
    xrGetInstanceProcAddr = next;
    const auto load = [&](const char* name, auto& fn) {
      PFN_xrVoidFunction raw = nullptr;
      if (XR_FAILED(next(instance, name, &raw))) {
        raw = nullptr;
      }
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(raw);
      return fn != nullptr;
    };
    mHasCore = true;
#define ODVR_LOAD_CORE(name) mHasCore = load(#name, name) && mHasCore;
    ODVR_NEXT_CORE_FUNCTIONS(ODVR_LOAD_CORE)
#undef ODVR_LOAD_CORE
#define ODVR_LOAD_OPTIONAL(name) load(#name, name);
    ODVR_NEXT_OPTIONAL_FUNCTIONS(ODVR_LOAD_OPTIONAL)
#undef ODVR_LOAD_OPTIONAL
    if (handTrackingEnabled) {
      mHasHandTracking = true;
#define ODVR_LOAD_HAND(name) mHasHandTracking = load(#name, name) && mHasHandTracking;
      ODVR_NEXT_HAND_FUNCTIONS(ODVR_LOAD_HAND)
#undef ODVR_LOAD_HAND
    }
  }
};

}// namespace OpenDesktopVR::Layer
