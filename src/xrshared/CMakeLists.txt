# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# The shared-memory ABI between the host and the layer (doc 02): layout,
# seqlocks and named mappings. Used by both sides.

odvr_module(
  xrshared
  SOURCES
  SharedMemory.cpp
  KnownLayers.cpp
  DEPS
  odvr::core
)
