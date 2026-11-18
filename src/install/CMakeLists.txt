# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Installing for one Windows user (M7a, ADR-043, ADR-009): versions, the
# setup program's payload, and the steps it takes (files, the Start menu,
# Apps and Features, the Restart Manager). No windows here: the setup
# program and the tests drive it. Windows' own libraries only: the
# Compression API, CNG for SHA-256, the Restart Manager, the shell.

odvr_module(
  install
  SOURCES
  Version.cpp
  Payload.cpp
  Installer.cpp
  DEPS
  odvr::settings
  odvr::core
  PRIVATE_DEPS
  cabinet
  bcrypt
  rstrtmgr
  ole32
  shell32
  advapi32
)
