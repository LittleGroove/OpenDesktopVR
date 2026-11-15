# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
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
