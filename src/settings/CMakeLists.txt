# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
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
