# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# The host's GPU side (doc 08 "Host side"): device on the game's adapter,
# Direct2D/DirectWrite, shared panel rings with fences, content sources.

odvr_module(
  render
  SOURCES
  ContentSource.cpp
  GpuDevice.cpp
  PanelRing.cpp
  TestPatternSource.cpp
  DEPS
  odvr::core
  odvr::cppwinrt
  d3d11
  dxgi
  d2d1
  dwrite
)
