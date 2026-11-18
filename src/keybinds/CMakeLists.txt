# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Keybinds (doc 09, KY-1 to KY-4): bindings, when they fire, bind capture,
# and the input thread watching the keyboard (Raw Input) and game
# controllers (DirectInput 8).

odvr_module(
  keybinds
  SOURCES
  Keybinds.cpp
  ChordTracker.cpp
  ControllerState.cpp
  BindCapture.cpp
  InputThread.cpp
  DEPS
  odvr::settings
  odvr::core
  odvr::cppwinrt
  PRIVATE_DEPS
  dinput8
  dxguid
  wtsapi32
  user32
  ole32
)
