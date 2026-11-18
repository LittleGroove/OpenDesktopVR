// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/AppHelperRules.hpp>

namespace OpenDesktopVR::AppHelper {

AppFamily FamilyOf(std::wstring_view windowClass) noexcept {
  if (windowClass.starts_with(L"Chrome_WidgetWin_")) {
    return AppFamily::Chromium;
  }
  if (windowClass == L"MozillaWindowClass") {
    return AppFamily::Firefox;
  }
  return AppFamily::None;
}

std::string_view FamilyName(AppFamily family) noexcept {
  switch (family) {
    case AppFamily::Chromium:
      return "Chromium-based";
    case AppFamily::Firefox:
      return "Firefox";
    case AppFamily::None:
      break;
  }
  return "other";
}

}// namespace OpenDesktopVR::AppHelper
