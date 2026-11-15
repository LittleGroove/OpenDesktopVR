# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
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
  DEPS
  odvr::cppwinrt
  shell32
)
