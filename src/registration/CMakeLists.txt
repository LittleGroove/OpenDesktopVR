# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Registering the layer with the OpenXR loader (per user, under HKCU).

odvr_module(
  registration
  SOURCES
  LayerRegistration.cpp
  DEPS
  odvr::core
  odvr::settings
  advapi32
)
