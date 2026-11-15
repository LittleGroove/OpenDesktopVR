// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The product's theme for Dear ImGui (ADR-020, ADR-023): the palette from
// Theme.hpp, rounded corners, and Segoe UI scaled for the monitor's DPI.
// Used by the desktop settings window now and by menus in VR later.
#pragma once

struct ImFont;

namespace OpenDesktopVR::UiTheme {

// Colours, spacing and rounding, scaled for `dpiScale` (1.0 = 96 DPI).
// Call after ImGui::CreateContext.
void Apply(float dpiScale);

// The body font (Segoe UI from the Windows fonts folder; ImGui's built-in
// font if it can't be loaded) and a larger one for headings
struct Fonts {
  ImFont* mBody {nullptr};
  ImFont* mHeading {nullptr};
};
Fonts LoadFonts(float dpiScale);

// Colours as ImGui wants them (0xAABBGGRR)
unsigned int Color(unsigned int rgb, float alpha = 1.0f) noexcept;

}// namespace OpenDesktopVR::UiTheme
