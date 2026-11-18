// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// JSON helpers for settings and any other untrusted JSON (TH-9): parsing and
// reading never throw, wrong types read as "absent", and one bad array entry
// is skipped instead of failing the whole list (ST-4).
#pragma once

#include <nlohmann/json.hpp>

#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::JsonUtil {

// Keeps key order, so hand-edited files stay readable after a round trip
using Json = nlohmann::ordered_json;

std::optional<Json> Parse(std::string_view text) noexcept;

// Pretty-printed, invalid UTF-8 replaced rather than thrown on
std::string Dump(const Json& value, int indent = 2) noexcept;

// The value of `key` if `object` is an object holding a value of type T
// (integers must fit T); nullopt otherwise
template <class T>
std::optional<T> Get(const Json& object, std::string_view key) noexcept {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto it = object.find(key);
  if (it == object.end()) {
    return std::nullopt;
  }
  const Json& value = *it;
  // Every branch returns, so no code is unreachable for any T (C4702)
  if constexpr (std::same_as<T, Json>) {
    return value;
  } else if constexpr (std::same_as<T, bool>) {
    if (value.is_boolean()) {
      return value.get<bool>();
    }
    return std::nullopt;
  } else if constexpr (std::integral<T>) {
    if (value.is_number_integer()) {
      if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        if (v <= static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
          return static_cast<T>(v);
        }
      } else {
        const auto v = value.get<std::int64_t>();
        if (
          v >= static_cast<std::int64_t>(std::numeric_limits<T>::min())
          && (v < 0 || static_cast<std::uint64_t>(v) <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()))) {
          return static_cast<T>(v);
        }
      }
    }
    return std::nullopt;
  } else if constexpr (std::floating_point<T>) {
    if (value.is_number()) {
      return static_cast<T>(value.get<double>());
    }
    return std::nullopt;
  } else if constexpr (std::same_as<T, std::string>) {
    if (value.is_string()) {
      return value.get<std::string>();
    }
    return std::nullopt;
  } else {
    static_assert(sizeof(T) == 0, "unsupported type");
  }
}

template <class T>
T GetOr(const Json& object, std::string_view key, T fallback) noexcept {
  if (auto value = Get<T>(object, key)) {
    return std::move(*value);
  }
  return fallback;
}

// Parses each element of an array with `parse` (returning std::optional<T>).
// Elements that parse to nullopt, or throw, are skipped and counted in
// `skipped` (ST-4: one bad panel entry doesn't fail the profile).
template <class T, class ParseElement>
std::vector<T> ParseEach(const Json& array, ParseElement&& parse, std::size_t* skipped = nullptr) noexcept {
  std::vector<T> result;
  std::size_t bad = 0;
  if (array.is_array()) {
    for (const auto& element: array) {
      try {
        if (std::optional<T> parsed = parse(element)) {
          result.push_back(std::move(*parsed));
          continue;
        }
      } catch (...) {
      }
      ++bad;
    }
  }
  if (skipped) {
    *skipped = bad;
  }
  return result;
}

// Adds every top-level key of `defaults` that `document` lacks. Keys
// `document` already has, including unknown ones, are left alone (ST-5).
void FillMissing(Json& document, const Json& defaults) noexcept;

}// namespace OpenDesktopVR::JsonUtil
