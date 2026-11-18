# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Direct3D 11 helpers shared by the layer and the host, and saving images
# (screenshots and test evidence).

odvr_module(
  gfx
  SOURCES
  D3D11Util.cpp
  ImageFile.cpp
  DEPS
  odvr::cppwinrt
  d3d11
  windowscodecs
  ole32
)
