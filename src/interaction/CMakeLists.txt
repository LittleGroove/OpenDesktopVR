# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Pure hand-interaction logic (ported from the prototype): pointing, tap,
# hold, drag, scroll, and fingertip typing. No OpenXR or graphics.

odvr_module(
  interaction
  PORTED
  SOURCES
  HandInteraction.cpp
  HandTyping.cpp
  DEPS
  odvr::simplemath
)
