# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
#
# Key-message building and caret detection (ported from the prototype).

odvr_module(
  inject
  PORTED
  SOURCES
  WindowKeyInjection.cpp
  TextInputMonitor.cpp
  DEPS
  odvr::cppwinrt
  user32
  oleacc
  ole32
)
