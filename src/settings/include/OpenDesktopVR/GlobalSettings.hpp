// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// settings.json, the global settings (doc 09 "Settings storage"): one spec
// for everything that reads or writes it (the host, odvr-layer), so they
// agree on its schema.
//
// Versions:
// 1: everything, the panel list included ("panels").
// 2: the panel list moved to profiles (ADR-038). The move itself is the
//    host's, keyed on "panels" still being there, so the version 1 to 2
//    migration changes nothing; the store keeps a backup of the version 1
//    file before it (ST-5).
#pragma once

#include <OpenDesktopVR/SettingsStore.hpp>

namespace OpenDesktopVR::Settings {

inline constexpr int GlobalSchemaVersion = 2;

DocumentSpec GlobalSettingsSpec();

}// namespace OpenDesktopVR::Settings
