# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
#
# Taps, drags, hover and scrolling from the hands into captured windows, as
# window messages (doc 06; M4b).

odvr_module(
  pointer
  SOURCES
  PointerInjector.cpp
  DEPS
  odvr::interaction
  odvr::apphelper_protocol
  odvr::inject
  odvr::core
  user32
)
