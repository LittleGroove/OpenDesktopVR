// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Showing and hiding panels (doc 09 "Visibility rules", PN-6):
//
// - Enabled (stored, the settings window) means "this panel exists in VR";
//   hidden (runtime only: keybinds and the tray) hides it for now, and is
//   forgotten when the host restarts. A panel shows when it's enabled and
//   not hidden. A hidden panel keeps its capture, so showing it is instant
//   (CP-5).
// - Show or hide all: if any panel is showing, hide all of them; if none
//   is, clear every hide; if none is even enabled, enable them all, so the
//   toggle never seems to do nothing.
// - Show or hide one panel while all are hidden shows just that one (it
//   follows from the rules above).
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>

#include <cstdint>
#include <set>
#include <span>

namespace OpenDesktopVR::Host::Visibility {

using Hidden = std::set<uint64_t>;

bool IsShowing(const PanelDefinition& panel, const Hidden& hidden) noexcept;
bool AnyShowing(std::span<const PanelDefinition> panels, const Hidden& hidden) noexcept;

struct ToggleAllResult {
  Hidden mHidden;
  bool mEnableAll {false};// none was enabled: enable them all (stored)
};
ToggleAllResult ToggleAll(std::span<const PanelDefinition> panels, const Hidden& hidden);

// Flips one panel's hide; a panel that isn't enabled is left alone
Hidden TogglePanel(std::span<const PanelDefinition> panels, const Hidden& hidden, uint64_t panelID);

// Hides of panels that no longer exist are dropped
Hidden Prune(std::span<const PanelDefinition> panels, const Hidden& hidden);

}// namespace OpenDesktopVR::Host::Visibility
