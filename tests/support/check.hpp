// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// A tiny self-checking test harness for the new tests. It prints in the same
// shape as the ported tests ("N checks, M failed"), so CI logs read alike.
//
//   int main() {
//     Scenario("Something works", [] { Check(1 + 1 == 2, "maths"); });
//     return Finish();
//   }
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <string>

namespace OpenDesktopVR::Testing {

inline int gChecks = 0;
inline int gFailures = 0;

inline bool Check(bool condition, const std::string& what) {
  ++gChecks;
  if (!condition) {
    ++gFailures;
  }
  std::printf("  [%s] %s\n", condition ? "pass" : "FAIL", what.c_str());
  return condition;
}

inline void Scenario(const char* name, const std::function<void()>& body) {
  std::printf("\n%s\n", name);
  try {
    body();
  } catch (const std::exception& e) {
    Check(false, std::string {"threw: "} + e.what());
  } catch (...) {
    Check(false, "threw an unknown exception");
  }
}

inline int Finish() {
  std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}

// A fresh, empty folder under %TEMP% for one test run, removed afterwards
class TempFolder {
 public:
  TempFolder();
  ~TempFolder();
  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;
  const std::wstring& Path() const noexcept {
    return mPath;
  }

 private:
  std::wstring mPath;
};

}// namespace OpenDesktopVR::Testing

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <format>

inline OpenDesktopVR::Testing::TempFolder::TempFolder() {
  static std::atomic<int> sCounter {0};
  std::error_code ec;
  const auto root = std::filesystem::temp_directory_path(ec);
  mPath = (root / std::format(L"odvr-test-{}-{}", GetCurrentProcessId(), sCounter++)).wstring();
  std::filesystem::remove_all(mPath, ec);
  std::filesystem::create_directories(mPath, ec);
}

inline OpenDesktopVR::Testing::TempFolder::~TempFolder() {
  std::error_code ec;
  std::filesystem::remove_all(mPath, ec);
}
