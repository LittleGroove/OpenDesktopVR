// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Hand settings in settings.json (doc 09 "Settings storage"):
//
//   "hands": {"enabled": true, "showWhilePointing": true,
//             "tapOffsetUpMillimeters": 0, "reverseScroll": false}
//
// Reading is tolerant (ST-4): a missing or malformed value reads as its
// default, an odd offset is brought into range. Keys this version doesn't
// know survive a write (ST-5).
#pragma once

#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>

namespace OpenDesktopVR::Host::HandConfig {

HandSettings Read(const JsonUtil::Json& settings) noexcept;
void Write(JsonUtil::Json& settings, const HandSettings& hands);

}// namespace OpenDesktopVR::Host::HandConfig
