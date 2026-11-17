# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# The virtual keyboard (doc 07, ADR-036).
# - vkeyboard: its logic (geometry, keys, when it shows) and what it draws,
#   shared between threads; no graphics, so tests run anywhere.
# - vkeyboardui: drawing it (Direct2D) and its click sound (XAudio2).

odvr_module(
  vkeyboard
  SOURCES
  KeyboardGeometry.cpp
  VirtualKeyboard.cpp
  KeyboardAutoShow.cpp
  KeyboardModel.cpp
  DEPS
  odvr::keyboard
  odvr::inject
  odvr::core
  user32
)

odvr_module(
  vkeyboardui
  SOURCES
  KeyboardSource.cpp
  ClickSound.cpp
  DEPS
  odvr::vkeyboard
  odvr::render
  odvr::core
  d2d1
  dwrite
  xaudio2
)
