# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# OpenXR helpers shared by the layer, the host and tools.

odvr_module(
  xrutil
  SOURCES
  XrUtil.cpp
  DEPS
  odvr::openxr_headers
)
