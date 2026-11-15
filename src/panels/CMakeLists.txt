# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Panel model and policies (doc 09, doc 08 "Update-rate policy"). Pure
# logic: no graphics, no Windows UI.

odvr_module(
  panels
  SOURCES
  UpdatePolicy.cpp
  DEPS
  odvr::core
)
