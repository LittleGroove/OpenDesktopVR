# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
#
# See-through hand renderer and its shaders (ported from the prototype).

odvr_module(
  handrender
  PORTED
  SOURCES
  HandRenderer.cpp
  DEPS
  odvr::interaction
  odvr::cppwinrt
  d3d11
)
odvr_shaders(
  odvr_handrender
  HLSL shaders/Hand.hlsl
  ENTRIES
  "HandConeVS|vs_5_0|HandConeVertexShader"
  "HandSphereVS|vs_5_0|HandSphereVertexShader"
  "HandPS|ps_5_0|HandPixelShader"
)
