// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Open Desktop VR host app (doc 02): a tray app, single instance, that
// renders panels and shares them with the layer in each game.
//
//   OpenDesktopVR.exe [--no-register] [--exit-after <s>] [--fence-delay-ms <n>] [--static-panels]
//                     [--window <title>] [--warp] [--settings] [--settings-screenshot <png>]
//                     [--picker-screenshot <png>]
//
// --no-register      don't register the layer (tests, side-by-side builds)
// --exit-after <s>   quit after this many seconds (tests)
// --fence-delay-ms   test only: announce frames before their GPU work is
//                    signalled, to exercise the layer's bounded staleness
// --static-panels    test panels that never change (the idle bench gate)
// --window <title>   test instances: one window panel, matching this title
// --warp             allow software rendering if there's no GPU (CI)
// --settings         open the settings window at start
// --settings-screenshot <png>, --picker-screenshot <png>
//                    tests: open the settings window (with the window
//                    picker), save it as a PNG, check that it stops
//                    painting when idle, then quit (SettingsWindow.hpp)

#include "PanelManager.hpp"
#include "SettingsWindow.hpp"
#include "TrayIcon.hpp"

#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/TestPatternSource.hpp>
#include <OpenDesktopVR/WindowCaptureSource.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <Windows.h>

#include <shellapi.h>

#include <cwchar>
#include <string_view>

using namespace OpenDesktopVR;

namespace {

struct Options {
  bool mRegister {true};
  double mExitAfterSeconds {0};
  int mFenceDelayMs {0};
  bool mAllowWarp {false};
  bool mStaticPanels {false};
  bool mTestInstance {false};
  std::wstring mTestWindowTitle;
  bool mOpenSettings {false};
  std::wstring mScreenshot;
  bool mScreenshotPicker {false};
};

Options ParseOptions() {
  Options options;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring_view arg {argv[i]};
    if (arg == L"--no-register") {
      options.mRegister = false;
    } else if (arg == L"--exit-after" && i + 1 < argc) {
      options.mExitAfterSeconds = std::wcstod(argv[++i], nullptr);
    } else if (arg == L"--fence-delay-ms" && i + 1 < argc) {
      options.mFenceDelayMs = std::max(0, _wtoi(argv[++i]));
    } else if (arg == L"--window" && i + 1 < argc) {
      options.mTestWindowTitle = argv[++i];
    } else if (arg == L"--static-panels") {
      options.mStaticPanels = true;
    } else if (arg == L"--warp") {
      options.mAllowWarp = true;
    } else if (arg == L"--settings") {
      options.mOpenSettings = true;
    } else if ((arg == L"--settings-screenshot" || arg == L"--picker-screenshot") && i + 1 < argc) {
      options.mScreenshotPicker = arg == L"--picker-screenshot";
      options.mScreenshot = argv[++i];
      options.mOpenSettings = true;
    }
  }
  LocalFree(argv);
  // A test instance never touches the real registration
  if (const auto instance = ProcessUtil::GetEnvironment("OPENDESKTOPVR_INSTANCE"); instance && !instance->empty()) {
    options.mRegister = false;
    options.mTestInstance = true;// built-in test panels; the user's settings untouched
  }
  return options;
}

// Registers the layer next to this exe on every start (doc 02 "Process
// lifecycle"); repairs a moved install and disables our stale entries
void RegisterLayer() {
  const auto manifest = ProcessUtil::ExecutablePath().parent_path() / Product::LayerManifestName;
  const auto result = Registration::Register(manifest);
  if (result.mOk) {
    Log::Info(
      "Layer registered: {}{}",
      StringUtil::Narrow(manifest.wstring()),
      result.mStaleDisabled ? " (older entries of ours disabled)" : "");
  } else {
    Log::Warning("Couldn't register the layer ({}): {}", result.mError, StringUtil::Narrow(manifest.wstring()));
  }
}

}// namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Window rectangles and capture crops in physical pixels (doc 04 "DPI")
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto options = ParseOptions();

  // Single instance (doc 02)
  const auto mutexName = SharedObjectName(L"Host", SharedAbi::AbiVersion);
  HANDLE single = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;// already running: its tray icon is the way in
  }

  Log::Config logConfig;
  if (const auto folder = Product::LogsFolder(); !folder.empty()) {
    logConfig.mFile = folder / L"host.log";
  }
  Log::Start(logConfig);

  // Elevation and UAC are the top cause of support requests (CMP-8)
  const auto uac = ProcessUtil::UacEnabled();
  Log::Info(
    "Host process: {}; token: {}; UAC: {}",
    ProcessUtil::ElevationName(ProcessUtil::CurrentProcessElevation()),
    ProcessUtil::CurrentElevationType(),
    uac ? (*uac ? "on" : "off") : "unknown");

  int exitCode = 0;
  ThreadUtil::CatchAll("host main", [&] {
    if (options.mRegister) {
      RegisterLayer();
    }

    Host::RenderLoopOptions loopOptions;
    loopOptions.mAllowWarp = options.mAllowWarp;
    loopOptions.mFenceDelay = std::chrono::milliseconds(options.mFenceDelayMs);
    if (options.mFenceDelayMs > 0) {
      Log::Warning("Test mode: frames are announced {} ms before their GPU work is signalled", options.mFenceDelayMs);
    }
    // What each kind of panel shows
    const bool staticPanels = options.mStaticPanels;
    Host::RenderLoop renderer {loopOptions,
      [staticPanels](const Host::PanelDefinition& definition, Render::GpuDevice& gpu, Render::MarkDirty markDirty)
        -> std::unique_ptr<Render::ContentSource> {
        if (definition.mKind == Host::PanelKind::Window) {
          return std::make_unique<Capture::WindowCaptureSource>(
            gpu, Capture::WindowCaptureOptions {definition.mWindow, definition.mName}, std::move(markDirty));
        }
        return std::make_unique<Render::TestPatternSource>(
          gpu, Render::TestPatternOptions {.mTitle = definition.mName, .mAnimated = definition.mAnimated && !staticPanels});
      }};
    if (!Capture::IsCaptureSupported()) {
      Log::Warning("Windows.Graphics.Capture isn't available here: window panels can't show");
    }
    std::vector<Host::PanelDefinition> testPanels;
    if (!options.mTestWindowTitle.empty()) {
      WindowUtil::WindowInfo match {nullptr, options.mTestWindowTitle};
      auto panel = Host::NewWindowPanel(match, 0);
      panel.mWindow = {L"", L"", options.mTestWindowTitle, WindowMatch::TitleMode::Contains};
      panel.mPosition[0] = 0;
      testPanels.push_back(std::move(panel));
    }
    Host::PanelManager panels {renderer, !options.mTestInstance, std::move(testPanels)};
    panels.Load();
    renderer.Start();

    Host::SettingsWindow settings {instance, renderer, panels};
    Host::TrayIcon tray {instance, renderer, panels, settings};
    if (options.mExitAfterSeconds > 0) {
      tray.QuitAfter(options.mExitAfterSeconds);
    }
    if (!options.mScreenshot.empty()) {
      settings.RequestScreenshot(options.mScreenshot, options.mScreenshotPicker);
    }
    if (options.mOpenSettings) {
      settings.Show();
    }
    exitCode = tray.Run();
    renderer.Stop();
  });

  Log::Info("Host exiting");
  Log::Stop();
  ReleaseMutex(single);
  CloseHandle(single);
  return exitCode;
}
