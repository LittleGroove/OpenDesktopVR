# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
#
# Updates from the project's GitHub Releases (M7, ADR-045): which release to
# offer (releases, or pre-releases too for the beta), and a small HTTPS
# client on Windows' WinHTTP. The host's UpdateService drives them.

odvr_module(
  update
  SOURCES
  Releases.cpp
  Http.cpp
  DEPS
  odvr::install
  odvr::settings
  odvr::core
  PRIVATE_DEPS
  winhttp
)
