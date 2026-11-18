// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Visibility.hpp>

#include <algorithm>

namespace OpenDesktopVR::Host::Visibility {

bool IsShowing(const PanelDefinition& panel, const Hidden& hidden) noexcept {
  return panel.mEnabled && !hidden.contains(panel.mID);
}

bool AnyShowing(std::span<const PanelDefinition> panels, const Hidden& hidden) noexcept {
  return std::ranges::any_of(panels, [&](const PanelDefinition& panel) { return IsShowing(panel, hidden); });
}

ToggleAllResult ToggleAll(std::span<const PanelDefinition> panels, const Hidden& hidden) {
  ToggleAllResult result;
  if (AnyShowing(panels, hidden)) {
    for (const auto& panel: panels) {
      if (panel.mEnabled) {
        result.mHidden.insert(panel.mID);
      }
    }
    return result;
  }
  // Nothing showing: every hide goes; if nothing is even enabled, all are
  result.mEnableAll = !panels.empty() && std::ranges::none_of(panels, &PanelDefinition::mEnabled);
  return result;
}

Hidden TogglePanel(std::span<const PanelDefinition> panels, const Hidden& hidden, uint64_t panelID) {
  const auto panel = std::ranges::find(panels, panelID, &PanelDefinition::mID);
  if (panel == panels.end() || !panel->mEnabled) {
    return hidden;
  }
  auto result = hidden;
  if (!result.erase(panelID)) {
    result.insert(panelID);
  }
  return result;
}

Hidden Prune(std::span<const PanelDefinition> panels, const Hidden& hidden) {
  Hidden result;
  for (const auto id: hidden) {
    if (std::ranges::find(panels, id, &PanelDefinition::mID) != panels.end()) {
      result.insert(id);
    }
  }
  return result;
}

}// namespace OpenDesktopVR::Host::Visibility
