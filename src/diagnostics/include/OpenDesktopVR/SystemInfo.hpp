// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Facts about this PC for the debug bundle and the startup log (doc 09
// "Debug bundle", "Environment checks at startup"): Windows' edition and
// build, the OpenXR runtime, the GPUs and their drivers and memory,
// hardware GPU scheduling, and environment problems (a Windows N edition
// without media components, compatibility mode, Wine, the code page).
//
// Every function reads only, never throws, and answers "unknown" when
// something can't be read. Environment problems are reported, never
// blocking (doc 09: never hard-block under Wine).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::Diagnostics {

// "Windows 11 Pro 24H2 (build 26100.2605)"
std::string WindowsVersion() noexcept;

// The active OpenXR runtime's manifest, from each place the loader may
// read it: "HKLM (64-bit): C:\...\runtime.json", and XR_RUNTIME_JSON if set
std::vector<std::string> ActiveRuntimes() noexcept;

struct GpuInfo {
  std::string mName;
  std::string mDriverVersion;// "32.0.15.6094", or "unknown"
  uint64_t mDedicatedMemory {0};
  std::optional<uint64_t> mBudget;// this process's local video memory budget
  std::optional<uint64_t> mUsage; // and its use of it
  bool mSoftware {false};
};
std::vector<GpuInfo> ListGpus() noexcept;

// Hardware-accelerated GPU scheduling: nullopt if not set (the driver's
// default) or unreadable
std::optional<bool> HardwareGpuScheduling() noexcept;

struct EnvironmentCheck {
  std::vector<std::string> mFacts;   // logged as Info
  std::vector<std::string> mProblems;// logged as Warning: explained, never blocking
};
EnvironmentCheck CheckEnvironment() noexcept;

// All of the above as text, one fact a line
std::string DescribeSystem() noexcept;

}// namespace OpenDesktopVR::Diagnostics
