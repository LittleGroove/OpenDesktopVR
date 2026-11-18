# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# The in-VR quick menu's logic (M6d, ADR-041): pages, layout, touches. No
# drawing and no Windows calls, so the simulator can use it to aim.

odvr_module(
  quickmenu
  SOURCES
  QuickMenu.cpp
)
