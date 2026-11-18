// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The product's theme for Dear ImGui (ADR-020, ADR-023): the palette from
// Theme.hpp, rounded corners, and Segoe UI scaled for the monitor's DPI.
// Used by the desktop settings window now and by menus in VR later.
#pragma once

struct ImFont;
struct ImFontAtlas;

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
// Into the current context's atlas, made its default font
Fonts LoadFonts(float dpiScale);
// Into `atlas` (cleared first). An atlas needs no ImGui context, so it can
// be built on another thread and shared with one later.
Fonts LoadFonts(ImFontAtlas& atlas, float dpiScale);

// Colours as ImGui wants them (0xAABBGGRR)
unsigned int Color(unsigned int rgb, float alpha = 1.0f) noexcept;

}// namespace OpenDesktopVR::UiTheme
