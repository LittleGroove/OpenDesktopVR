# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# User interface pieces shared by the desktop settings window and (later)
# menus in VR: the Dear ImGui theme (ADR-020, ADR-023).

odvr_module(
  ui
  SOURCES
  UiTheme.cpp
  DEPS
  odvr::imgui
  odvr::core
  shell32
)
