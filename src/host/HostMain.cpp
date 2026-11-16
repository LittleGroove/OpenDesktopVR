// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Open Desktop VR host app (doc 02): a tray app, single instance, that
// renders panels and shares them with the layer in each game.
//
//   OpenDesktopVR.exe [--no-register] [--exit-after <s>] [--fence-delay-ms <n>] [--static-panels]
//                     [--resizing-panels] [--window <title>] [--warp] [--settings]
//                     [--settings-screenshot <png>] [--picker-screenshot <png>] [--settings-delay <ms>]
//
// --no-register      don't register the layer (tests, side-by-side builds)
// --exit-after <s>   quit after this many seconds (tests)
// --fence-delay-ms   test only: announce frames before their GPU work is
//                    signalled, to exercise the layer's bounded staleness
// --static-panels    test panels that never change (the idle bench gate)
// --resizing-panels  test panels that change size every frame (RN-7)
// --window <title>   test instances: a window panel matching this title
//                    (repeat it for more)
// --warp             allow software rendering if there's no GPU (CI)
// --settings         open the settings window at start
// --settings-screenshot <png>, --picker-screenshot <png>
//                    tests: open the settings window (with the window
//                    picker), save it as a PNG, check that it stops
//                    painting when idle, then quit (SettingsWindow.hpp)
// --settings-delay <ms>  tests: open the settings window this long after
//                    start, as a user would (its device is made by then)

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

#include <algorithm>
#include <cwchar>
#include <string_view>
#include <vector>

using namespace OpenDesktopVR;

namespace {

struct Options {
  bool mRegister {true};
  double mExitAfterSeconds {0};
  int mFenceDelayMs {0};
  bool mAllowWarp {false};
  bool mStaticPanels {false};
  bool mResizingPanels {false};
  bool mTestInstance {false};
  std::vector<std::wstring> mTestWindowTitles;
  bool mOpenSettings {false};
  int mSettingsDelayMs {0};
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
      options.mTestWindowTitles.emplace_back(argv[++i]);
    } else if (arg == L"--static-panels") {
      options.mStaticPanels = true;
    } else if (arg == L"--resizing-panels") {
      options.mResizingPanels = true;
    } else if (arg == L"--warp") {
      options.mAllowWarp = true;
    } else if (arg == L"--settings") {
      options.mOpenSettings = true;
    } else if (arg == L"--settings-delay" && i + 1 < argc) {
      options.mSettingsDelayMs = std::clamp(_wtoi(argv[++i]), 0, 10'000);
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
    const bool resizingPanels = options.mResizingPanels;
    Host::RenderLoop renderer {loopOptions,
      [staticPanels, resizingPanels](const Host::PanelDefinition& definition, Render::GpuDevice& gpu, Render::MarkDirty markDirty)
        -> std::unique_ptr<Render::ContentSource> {
        if (definition.mKind == Host::PanelKind::Window) {
          return std::make_unique<Capture::WindowCaptureSource>(
            gpu, Capture::WindowCaptureOptions {definition.mWindow, definition.mName}, std::move(markDirty));
        }
        return std::make_unique<Render::TestPatternSource>(
          gpu, Render::TestPatternOptions {
            .mTitle = definition.mName,
            .mAnimated = (definition.mAnimated && !staticPanels) || resizingPanels,
            .mResizing = resizingPanels,
          });
      }};
    if (!Capture::IsCaptureSupported()) {
      Log::Warning("Windows.Graphics.Capture isn't available here: window panels can't show");
    }
    std::vector<Host::PanelDefinition> testPanels;
    for (const auto& title: options.mTestWindowTitles) {
      WindowUtil::WindowInfo match {nullptr, title};
      auto panel = Host::NewWindowPanel(match, testPanels.size());
      panel.mWindow = {L"", L"", title, WindowMatch::TitleMode::Contains};
      if (options.mTestWindowTitles.size() == 1) {
        panel.mPosition[0] = 0;
      }
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
      // Tests only: the message loop hasn't started, so nothing waits on this
      Sleep(static_cast<DWORD>(options.mSettingsDelayMs));
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
