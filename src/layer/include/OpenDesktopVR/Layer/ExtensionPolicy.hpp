// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Which extensions the layer adds to the game's xrCreateInstance (XR-7).
// Hand tracking is probed in three states; "unknown" requests it anyway and
// the caller retries without it on XR_ERROR_EXTENSION_NOT_PRESENT.
#pragma once

#include <openxr/openxr.h>

#include <span>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Layer {

enum class Support { Supported, Unsupported, Unknown };

std::string_view SupportName(Support support) noexcept;

// Asks the next layer (or runtime) for its instance extensions, before any
// instance exists. A failed lookup or enumeration is Unknown, never "no":
// some layers below can't answer until their own instance exists.
Support ProbeInstanceExtension(PFN_xrGetInstanceProcAddr next, std::string_view extension) noexcept;

struct ExtensionPlan {
  // Pointers into the game's list, plus static names we add
  std::vector<const char*> mNames;
  bool mGameEnabledHandTracking {false};
  bool mLayerAddsHandTracking {false};
  // Log for "who enabled it". We only see what reaches us, so a request
  // from the game and one from a layer above us look the same.
  std::string_view Who() const noexcept;
};

ExtensionPlan PlanExtensions(std::span<const char* const> gameExtensions, Support handTracking);

// The game's own list again, for the retry after XR_ERROR_EXTENSION_NOT_PRESENT
ExtensionPlan WithoutLayerAdditions(std::span<const char* const> gameExtensions);

bool Contains(std::span<const char* const> names, std::string_view name) noexcept;

}// namespace OpenDesktopVR::Layer
