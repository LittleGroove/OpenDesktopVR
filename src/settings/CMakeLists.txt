# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Settings store: versioned JSON documents with debounced atomic writes
# (ADR-010, doc 09 "Settings storage", conditions ST-1 to ST-5).

odvr_module(
  settings
  SOURCES
  JsonUtil.cpp
  SettingsStore.cpp
  GlobalSettings.cpp
  DEPS
  odvr::core
  odvr::json
)
