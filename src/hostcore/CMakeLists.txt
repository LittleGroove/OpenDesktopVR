# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# The host app's logic, without its UI: panels, the render loop and the
# publisher. A library, so tests can run a host inside a test process.

odvr_module(
  hostcore
  SOURCES
  Publisher.cpp
  HostPanel.cpp
  PanelConfig.cpp
  RenderLoop.cpp
  Visibility.cpp
  HandService.cpp
  HandConfig.cpp
  InjectingSink.cpp
  KeyboardConfig.cpp
  KeyboardService.cpp
  ProfileLibrary.cpp
  GameFollower.cpp
  DEPS
  odvr::vkeyboard
  odvr::inject
  odvr::interaction
  odvr::pointer
  odvr::keybinds
  odvr::render
  odvr::capture
  odvr::panels
  odvr::xrshared
  odvr::core
  dxguid
)
