# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Desktop windows as panel content (doc 04 "Window capture"): window
# listing and match rules, and the Windows.Graphics.Capture source.

odvr_module(
  capture
  SOURCES
  WindowUtil.cpp
  WindowMatch.cpp
  WindowCaptureSource.cpp
  DEPS
  odvr::render
  odvr::gfx
  odvr::settings
  odvr::core
  odvr::cppwinrt
  dwmapi
  d3d11
  dxgi
  gdi32
)
odvr_shaders(
  odvr_capture
  HLSL shaders/Compare.hlsl
  ENTRIES
  "CompareCS|cs_5_0|CompareCS"
)
