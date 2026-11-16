# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# The host's side of the app helper (doc 06; ADR-031): which captured apps
# need it, and the service that loads and unloads it.

odvr_module(
  apphelperhost
  SOURCES
  AppHelperRules.cpp
  AppHelperService.cpp
  DEPS
  odvr::core
  PRIVATE_DEPS
  odvr::apphelper_protocol
  user32
)
# The helper DLL is built with it, so the host finds it next to its exe
add_dependencies(odvr_apphelperhost ${ODVR_APP_HELPER_FILE_STEM})
