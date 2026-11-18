# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
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
