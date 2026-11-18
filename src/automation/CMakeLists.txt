# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
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
