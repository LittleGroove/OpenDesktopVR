// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// What the layer learns about the game's OpenXR setup when its instance is
// created, for the log, the host's diagnostics page and the self-test
// (M6c; CMP-1, CMP-2, CMP-7):
// - the API layers between ours and the runtime, from the loader's chain
//   (each XrApiLayerNextInfo names the layer it goes to; ours is first);
// - whether the game enables XR_KHR_composition_layer_depth: with depth
//   submitted, some runtimes hide overlays behind the cockpit *(reported)*.
//
// Called once per instance, never per frame.
#pragma once

#include <OpenDesktopVR/KnownLayers.hpp>

#include <openxr/openxr.h>

#include <loader_interfaces.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Layer {

struct ChainSummary {
  std::vector<std::string> mBelow;// nearest ours first; the last is nearest the runtime
  uint32_t mKnownBelow {0};       // KnownLayers::Kind bits among them
  uint32_t mNearestRuntime {0};   // the last one's Kind, or 0

  // "A, B" or "none"
  std::string Describe() const;
};

// `ours` is the XrApiLayerNextInfo the loader gave our layer. Bounded: at
// most 64 entries are read, in case a chain is malformed.
ChainSummary SummarizeChain(const XrApiLayerNextInfo* ours, std::string_view ourName);

bool EnablesExtension(std::span<const char* const> extensions, std::string_view name) noexcept;

}// namespace OpenDesktopVR::Layer
