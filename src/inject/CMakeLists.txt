# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
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
