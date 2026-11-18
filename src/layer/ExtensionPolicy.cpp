// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Layer/ExtensionPolicy.hpp>

#include <vector>

namespace OpenDesktopVR::Layer {

std::string_view SupportName(Support support) noexcept {
  switch (support) {
    case Support::Supported:
      return "supported";
    case Support::Unsupported:
      return "unsupported";
    case Support::Unknown:
      return "unknown";
  }
  return "?";
}

bool Contains(std::span<const char* const> names, std::string_view name) noexcept {
  for (const auto* n: names) {
    if (n && name == n) {
      return true;
    }
  }
  return false;
}

Support ProbeInstanceExtension(PFN_xrGetInstanceProcAddr next, std::string_view extension) noexcept {
  if (!next) {
    return Support::Unknown;
  }
  PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
  if (
    XR_FAILED(next(
      XR_NULL_HANDLE,
      "xrEnumerateInstanceExtensionProperties",
      reinterpret_cast<PFN_xrVoidFunction*>(&enumerate)))
    || !enumerate) {
    return Support::Unknown;
  }
  try {
    uint32_t count = 0;
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)) || count > 4096) {
      return Support::Unknown;
    }
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(enumerate(nullptr, count, &count, properties.data()))) {
      return Support::Unknown;
    }
    for (uint32_t i = 0; i < count && i < properties.size(); ++i) {
      if (extension == properties[i].extensionName) {
        return Support::Supported;
      }
    }
    return Support::Unsupported;
  } catch (...) {
    return Support::Unknown;
  }
}

std::string_view ExtensionPlan::Who() const noexcept {
  if (mGameEnabledHandTracking) {
    return "the game or a layer above us";
  }
  if (mLayerAddsHandTracking) {
    return "the layer";
  }
  return "nobody";
}

ExtensionPlan PlanExtensions(std::span<const char* const> gameExtensions, Support handTracking) {
  ExtensionPlan plan;
  plan.mNames.assign(gameExtensions.begin(), gameExtensions.end());
  plan.mGameEnabledHandTracking = Contains(gameExtensions, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
  // Never add it twice; never add it when the runtime said no
  if (!plan.mGameEnabledHandTracking && handTracking != Support::Unsupported) {
    plan.mNames.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    plan.mLayerAddsHandTracking = true;
  }
  return plan;
}

ExtensionPlan WithoutLayerAdditions(std::span<const char* const> gameExtensions) {
  ExtensionPlan plan;
  plan.mNames.assign(gameExtensions.begin(), gameExtensions.end());
  plan.mGameEnabledHandTracking = Contains(gameExtensions, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
  return plan;
}

}// namespace OpenDesktopVR::Layer
