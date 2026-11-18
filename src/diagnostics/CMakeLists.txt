# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Diagnostics and support (M6c, doc 09): the "doesn't show in game"
# self-test, the debug bundle (a .zip writer, path redaction), and facts
# about the PC. Host side only: never linked into the layer.

odvr_module(
  diagnostics
  SOURCES
  ZipWriter.cpp
  Redaction.cpp
  SystemInfo.cpp
  SelfTest.cpp
  DebugBundle.cpp
  DEPS
  odvr::registration
  odvr::xrshared
  odvr::core
  odvr::cppwinrt
  dxgi
  shell32
  ole32
  advapi32
)
