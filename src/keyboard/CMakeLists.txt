# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Reads installed Windows keyboard layouts (ported from the prototype).

odvr_module(
  keyboard
  PORTED
  SOURCES
  KeyboardLayout.cpp
  DEPS
  user32
  shlwapi
)
