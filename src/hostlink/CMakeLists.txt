# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# The client side of the shared block (layer and tools): connect, follow
# host restarts, pull handles, open shared textures and fences.

odvr_module(
  hostlink
  SOURCES
  HostLink.cpp
  DEPS
  odvr::xrshared
  odvr::core
  odvr::cppwinrt
  d3d11
)
