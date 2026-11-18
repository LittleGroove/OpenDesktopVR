# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
#
# Logging, events, threading, and the shared utility files every other
# module builds on. Safe to link into the in-game layer: no JSON, no UI.

odvr_module(
  core
  SOURCES
  Log.cpp
  StringUtil.cpp
  FileUtil.cpp
  ThreadUtil.cpp
  Product.cpp
  ProcessUtil.cpp
  Autostart.cpp
  CrashHandler.cpp
  LogRing.cpp
  DEPS
  odvr::cppwinrt
  shell32
  advapi32
  dbghelp
)
