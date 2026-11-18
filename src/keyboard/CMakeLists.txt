# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
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
