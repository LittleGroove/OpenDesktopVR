# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# OpenDesktopVR.exe: the tray app (doc 02 "Host").

odvr_executable(
  OpenDesktopVR
  WIN32
  SOURCES
  HostMain.cpp
  TrayIcon.cpp
  PanelManager.cpp
  DEPS
  odvr::hostcore
  odvr::capture
  odvr::settings
  odvr::registration
  shell32
)
set_target_properties(OpenDesktopVR PROPERTIES FOLDER "host")
