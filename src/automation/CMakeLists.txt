# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Automation (M6e, UI-9, ADR-042): commands from Stream Deck, VoiceAttack and
# other tools, parsed and packed for the host. No windows or messages here.

odvr_module(
  automation
  SOURCES
  Automation.cpp
  DEPS
  odvr::core
)
