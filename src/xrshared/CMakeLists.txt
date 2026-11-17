# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
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
