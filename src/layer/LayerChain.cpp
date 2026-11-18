// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Layer/LayerChain.hpp>

#include <cstring>

namespace OpenDesktopVR::Layer {

namespace {
constexpr int MaxChainEntries = 64;

std::string_view NameOf(const XrApiLayerNextInfo& info) noexcept {
  return {info.layerName, strnlen(info.layerName, sizeof(info.layerName))};
}
}// namespace

std::string ChainSummary::Describe() const {
  if (mBelow.empty()) {
    return "none";
  }
  std::string text;
  for (const auto& name: mBelow) {
    if (!text.empty()) {
      text += ", ";
    }
    text += name;
  }
  return text;
}

ChainSummary SummarizeChain(const XrApiLayerNextInfo* ours, std::string_view ourName) {
  ChainSummary summary;
  if (!ours) {
    return summary;
  }
  // Ours is first; tolerate a loader that starts at the next one
  const XrApiLayerNextInfo* entry = NameOf(*ours) == ourName ? ours->next : ours;
  for (int count = 0; entry && count < MaxChainEntries; entry = entry->next, ++count) {
    if (entry->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO) {
      break;
    }
    const auto name = NameOf(*entry);
    summary.mBelow.emplace_back(name);
    const auto kind = KnownLayers::Classify(name);
    summary.mKnownBelow |= kind;
    summary.mNearestRuntime = kind;
  }
  return summary;
}

bool EnablesExtension(std::span<const char* const> extensions, std::string_view name) noexcept {
  for (const char* extension: extensions) {
    if (extension && name == extension) {
      return true;
    }
  }
  return false;
}

}// namespace OpenDesktopVR::Layer
