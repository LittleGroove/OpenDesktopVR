# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
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
  AutomationService.cpp
  UpdateService.cpp
  DEPS
  odvr::hostcore
  odvr::hostlink
  odvr::vkeyboardui
  odvr::keybinds
  odvr::capture
  odvr::documents
  odvr::apphelperhost
  odvr::settings
  odvr::registration
  odvr::diagnostics
  odvr::automation
  odvr::install
  odvr::update
  odvr::ui
  odvr::imgui
  odvr::gfx
  shell32
  dwmapi
)
set_target_properties(OpenDesktopVR PROPERTIES FOLDER "host")
