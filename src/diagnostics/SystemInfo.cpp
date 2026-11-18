// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/SystemInfo.hpp>

#include <Windows.h>

#include <dxgi1_4.h>

#include <winrt/base.h>

#include <format>

namespace OpenDesktopVR::Diagnostics {

namespace {

std::optional<std::wstring> ReadString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0) noexcept {
  wchar_t buffer[1024] {};
  DWORD size = sizeof(buffer) - sizeof(wchar_t);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : (view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : 0)),
        nullptr, buffer, &size)
      != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return std::wstring {buffer};
}

std::optional<DWORD> ReadDword(HKEY root, const wchar_t* key, const wchar_t* value) noexcept {
  DWORD data = 0;
  DWORD size = sizeof(data);
  if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return data;
}

std::string Megabytes(uint64_t bytes) {
  return std::format("{} MB", bytes >> 20);
}

}// namespace

std::string WindowsVersion() noexcept {
  try {
    constexpr auto Key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const auto product = ReadString(HKEY_LOCAL_MACHINE, Key, L"ProductName").value_or(L"Windows");
    const auto display = ReadString(HKEY_LOCAL_MACHINE, Key, L"DisplayVersion").value_or(L"");
    const auto build = ReadString(HKEY_LOCAL_MACHINE, Key, L"CurrentBuild").value_or(L"?");
    const auto edition = ReadString(HKEY_LOCAL_MACHINE, Key, L"EditionID").value_or(L"");
    const auto revision = ReadDword(HKEY_LOCAL_MACHINE, Key, L"UBR");
    auto name = StringUtil::Narrow(product);
    // Windows 11 still says "Windows 10" here; its builds start at 22000
    if (const int number = _wtoi(build.c_str()); number >= 22000 && name.starts_with("Windows 10")) {
      name.replace(0, 10, "Windows 11");
    }
    return std::format(
      "{}{}{} (build {}{}){}",
      name,
      display.empty() ? "" : " ",
      StringUtil::Narrow(display),
      StringUtil::Narrow(build),
      revision ? std::format(".{}", *revision) : std::string {},
      edition.empty() ? std::string {} : std::format(", edition {}", StringUtil::Narrow(edition)));
  } catch (...) {
    return "unknown";
  }
}

std::vector<std::string> ActiveRuntimes() noexcept {
  std::vector<std::string> runtimes;
  try {
    constexpr auto Key = L"SOFTWARE\\Khronos\\OpenXR\\1";
    for (const auto& [view, name]: {std::pair {KEY_WOW64_64KEY, "64-bit"}, std::pair {KEY_WOW64_32KEY, "32-bit"}}) {
      if (const auto runtime = ReadString(HKEY_LOCAL_MACHINE, Key, L"ActiveRuntime", view)) {
        runtimes.push_back(std::format("HKLM ({}): {}", name, StringUtil::Narrow(*runtime)));
      }
    }
    if (const auto runtime = ReadString(HKEY_CURRENT_USER, Key, L"ActiveRuntime")) {
      runtimes.push_back(std::format("HKCU: {}", StringUtil::Narrow(*runtime)));
    }
    if (const auto overridden = ProcessUtil::GetEnvironment("XR_RUNTIME_JSON")) {
      runtimes.push_back(std::format("XR_RUNTIME_JSON: {}", *overridden));
    }
  } catch (...) {
  }
  return runtimes;
}

std::vector<GpuInfo> ListGpus() noexcept {
  std::vector<GpuInfo> gpus;
  try {
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
      return gpus;
    }
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc {};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        GpuInfo gpu;
        gpu.mName = StringUtil::Narrow(desc.Description);
        gpu.mDedicatedMemory = desc.DedicatedVideoMemory;
        gpu.mSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        LARGE_INTEGER version {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
          gpu.mDriverVersion = std::format(
            "{}.{}.{}.{}",
            HIWORD(version.HighPart),
            LOWORD(version.HighPart),
            HIWORD(version.LowPart),
            LOWORD(version.LowPart));
        } else {
          gpu.mDriverVersion = "unknown";
        }
        if (const auto adapter3 = adapter.try_as<IDXGIAdapter3>()) {
          DXGI_QUERY_VIDEO_MEMORY_INFO memory {};
          if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            gpu.mBudget = memory.Budget;
            gpu.mUsage = memory.CurrentUsage;
          }
        }
        gpus.push_back(std::move(gpu));
      }
      adapter = nullptr;
    }
  } catch (...) {
  }
  return gpus;
}

std::optional<bool> HardwareGpuScheduling() noexcept {
  // HwSchMode: 2 on, 1 off; absent means the driver's default
  const auto mode = ReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode");
  if (!mode) {
    return std::nullopt;
  }
  return *mode == 2;
}

std::string ChosenCompatibilityModes(std::string_view compatLayer) {
  std::string chosen;
  std::size_t at = 0;
  while (at < compatLayer.size()) {
    const auto end = std::min(compatLayer.find(' ', at), compatLayer.size());
    const auto word = compatLayer.substr(at, end - at);
    at = end + 1;
    const bool detector = word.size() >= 9 && _strnicmp(word.data(), "Detectors", 9) == 0;
    if (word.empty() || word == "~" || detector) {
      continue;
    }
    chosen += (chosen.empty() ? "" : " ") + std::string {word};
  }
  return chosen;
}

EnvironmentCheck CheckEnvironment() noexcept {
  EnvironmentCheck check;
  try {
    check.mFacts.push_back(std::format("Code pages: ANSI {}, OEM {}", GetACP(), GetOEMCP()));
    if (const auto layer = ProcessUtil::GetEnvironment("__COMPAT_LAYER"); layer && !layer->empty()) {
      if (const auto chosen = ChosenCompatibilityModes(*layer); !chosen.empty()) {
        check.mProblems.push_back(std::format(
          "Running in compatibility mode ({}). Open Desktop VR doesn't need it: turn it off in the program's "
          "Properties, Compatibility tab",
          chosen));
      } else {
        check.mFacts.push_back(std::format("Windows' own compatibility shims: {} (nothing to change)", *layer));
      }
    }
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
      check.mProblems.push_back("Running under Wine: some features may not work; carrying on");
    }
    // Windows N editions lack Media Foundation unless the Media Feature
    // Pack is installed
    if (const HMODULE media = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      FreeLibrary(media);
    } else {
      check.mProblems.push_back(
        "Windows' media components are missing (a Windows N edition without the Media Feature Pack): video in "
        "captured apps may not play. Install the Media Feature Pack from Settings, Apps, Optional features");
    }
  } catch (...) {
  }
  return check;
}

std::string DescribeSystem() noexcept {
  try {
    std::string text;
    const auto line = [&text](std::string_view s) {
      text += s;
      text += "\r\n";
    };
    line(std::format("Open Desktop VR: {}", Product::BuildDescription()));
    line(std::format("Windows: {}", WindowsVersion()));
    const auto uac = ProcessUtil::UacEnabled();
    line(std::format(
      "This process: {}; token: {}; UAC: {}",
      ProcessUtil::ElevationName(ProcessUtil::CurrentProcessElevation()),
      ProcessUtil::CurrentElevationType(),
      uac ? (*uac ? "on" : "off") : "unknown"));
    const auto runtimes = ActiveRuntimes();
    if (runtimes.empty()) {
      line("OpenXR runtime: none set");
    }
    for (const auto& runtime: runtimes) {
      line(std::format("OpenXR runtime: {}", runtime));
    }
    for (const auto& gpu: ListGpus()) {
      line(std::format(
        "GPU: {}{}; driver {}; {} dedicated{}",
        gpu.mName,
        gpu.mSoftware ? " (software)" : "",
        gpu.mDriverVersion,
        Megabytes(gpu.mDedicatedMemory),
        gpu.mBudget ? std::format("; budget {}, this process uses {}", Megabytes(*gpu.mBudget), Megabytes(gpu.mUsage.value_or(0)))
                    : std::string {}));
    }
    const auto scheduling = HardwareGpuScheduling();
    line(std::format("Hardware-accelerated GPU scheduling: {}", scheduling ? (*scheduling ? "on" : "off") : "the driver's default"));
    const auto environment = CheckEnvironment();
    for (const auto& fact: environment.mFacts) {
      line(fact);
    }
    for (const auto& problem: environment.mProblems) {
      line(std::format("Problem: {}", problem));
    }
    return text;
  } catch (...) {
    return "(the system couldn't be described)\r\n";
  }
}

}// namespace OpenDesktopVR::Diagnostics
