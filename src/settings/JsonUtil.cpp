// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/JsonUtil.hpp>

namespace OpenDesktopVR::JsonUtil {

std::optional<Json> Parse(std::string_view text) noexcept {
  try {
    auto parsed = Json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (parsed.is_discarded()) {
      return std::nullopt;
    }
    return parsed;
  } catch (...) {
    return std::nullopt;
  }
}

std::string Dump(const Json& value, int indent) noexcept {
  try {
    return value.dump(indent, ' ', false, Json::error_handler_t::replace);
  } catch (...) {
    return {};
  }
}

void FillMissing(Json& document, const Json& defaults) noexcept {
  if (!document.is_object() || !defaults.is_object()) {
    return;
  }
  try {
    for (const auto& [key, value]: defaults.items()) {
      if (!document.contains(key)) {
        document[key] = value;
      }
    }
  } catch (...) {
  }
}

}// namespace OpenDesktopVR::JsonUtil
