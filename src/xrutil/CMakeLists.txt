# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# OpenXR helpers shared by the layer, the host and tools.

odvr_module(
  xrutil
  SOURCES
  XrUtil.cpp
  DEPS
  odvr::openxr_headers
)
