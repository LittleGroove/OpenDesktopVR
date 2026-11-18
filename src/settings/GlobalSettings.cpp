// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/GlobalSettings.hpp>
#include <OpenDesktopVR/Product.hpp>

namespace OpenDesktopVR::Settings {

DocumentSpec GlobalSettingsSpec() {
  return {
    .mPath = Product::SettingsFile(),
    .mCurrentVersion = GlobalSchemaVersion,
    .mDefaults = [] { return Json::object(); },
    .mMigrations = {{.mFromVersion = 1, .mApply = [](Json&) { return true; }}},
  };
}

}// namespace OpenDesktopVR::Settings
