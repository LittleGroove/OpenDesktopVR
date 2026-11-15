# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Direct3D 11 helpers shared by the layer and the host.

odvr_module(
  gfx
  SOURCES
  D3D11Util.cpp
  DEPS
  odvr::cppwinrt
  d3d11
)
