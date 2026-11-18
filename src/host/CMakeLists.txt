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
  SettingsWindow.cpp
  KeybindService.cpp
  Support.cpp
  QuickMenuController.cpp
  DEPS
  odvr::hostcore
  odvr::hostlink
  odvr::vkeyboardui
  odvr::keybinds
  odvr::capture
  odvr::apphelperhost
  odvr::settings
  odvr::registration
  odvr::diagnostics
  odvr::ui
  odvr::imgui
  odvr::gfx
  shell32
  dwmapi
)
set_target_properties(OpenDesktopVR PROPERTIES FOLDER "host")
