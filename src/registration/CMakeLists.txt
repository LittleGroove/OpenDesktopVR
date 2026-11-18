# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
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
