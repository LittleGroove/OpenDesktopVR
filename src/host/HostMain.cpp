// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Open Desktop VR host app (doc 02): a tray app, single instance, that
// renders panels and shares them with the layer in each game.
//
//   OpenDesktopVR.exe [--no-register] [--exit-after <s>] [--fence-delay-ms <n>] [--static-panels]
//                     [--resizing-panels] [--overlapping-panels] [--window <title>] [--warp] [--settings]
//                     [--settings-screenshot <png>] [--picker-screenshot <png>] [--settings-delay <ms>]
//
// --no-register      don't register the layer (tests, side-by-side builds)
// --exit-after <s>   quit after this many seconds (tests)
// --fence-delay-ms   test only: announce frames before their GPU work is
//                    signalled, to exercise the layer's bounded staleness
// --static-panels    test panels that never change (the idle bench gate)
// --resizing-panels  test panels that change size every frame (RN-7)
// --test-keybind <vk>  test instances: this key shows or hides all panels
// --test-look        the static test panel tinted red at half brightness
//                    (PN-5: its pixels are checked)
// --overlapping-panels  test instances: the test panels one in front of the
//                    other, nearest first (doc 11's depth-order scenario)
// --window <title>   test instances: a window panel matching this title
//                    (repeat it for more)
// --warp             allow software rendering if there's no GPU (CI)
// --settings         open the settings window at start
// --settings-screenshot <png>, --picker-screenshot <png>
//                    tests: open the settings window (with the window
//                    picker), save it as a PNG, check that it stops
//                    painting when idle, then quit (SettingsWindow.hpp)
// --settings-keybinds   tests: open the settings window on its Keybinds tab
// --settings-hands      tests: open the settings window on its Hands tab
// --settings-profiles   tests: open the settings window on its Profiles tab
// --settings-general    tests: open the settings window on its General tab
// --settings-cycle-tabs tests: show each tab once, one a frame, and report
//                    the longest page switch (DU-3)
// --settings-delay <ms>  tests: open the settings window this long after
//                    start, as a user would (its device is made by then)
// --show-keyboard    test instances: the keyboard shows from the start, as
//                    its keybind would show it (sim-keyboard)
// --test-recenter-after <s>  test instances: recenter this long after the
//                    start, as the keybind would (sim-recenter)
// --settings-diagnostics  tests: open the settings window on its
//                    Diagnostics tab
// --save-debug-info <zip>  tests: save the debug bundle there once
//                    started, as "Save debug info" would (M6c)

#include "KeybindService.hpp"
#include "PanelManager.hpp"
#include "QuickMenuController.hpp"
#include "SettingsWindow.hpp"
#include "Support.hpp"
#include "TrayIcon.hpp"

#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/ClickSound.hpp>
#include <OpenDesktopVR/CrashHandler.hpp>
#include <OpenDesktopVR/InjectingSink.hpp>
#include <OpenDesktopVR/KeyboardService.hpp>
#include <OpenDesktopVR/KeyboardSource.hpp>
#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/LogRing.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/QuickMenuHands.hpp>
#include <OpenDesktopVR/QuickMenuSource.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/SystemInfo.hpp>
#include <OpenDesktopVR/TestPatternSource.hpp>
#include <OpenDesktopVR/WindowCaptureSource.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <Windows.h>

#include <shellapi.h>

#include <algorithm>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>
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
  bool mTestLook {false};
  bool mOverlappingPanels {false};
  UINT mTestKeybind {0};
  bool mTestInstance {false};
  std::vector<std::wstring> mTestWindowTitles;
  bool mOpenSettings {false};
  int mSettingsDelayMs {0};
  bool mSettingsKeybinds {false};
  bool mSettingsHands {false};
  bool mSettingsProfiles {false};
  bool mSettingsCycleTabs {false};
  bool mSettingsGeneral {false};
  bool mSettingsDiagnostics {false};
  std::wstring mDebugInfoZip;
  bool mShowKeyboard {false};
  bool mShowQuickMenu {false};
  double mTestRecenterAfterSeconds {0};
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
    } else if (arg == L"--test-look") {
      options.mTestLook = true;
    } else if (arg == L"--overlapping-panels") {
      options.mOverlappingPanels = true;
    } else if (arg == L"--test-keybind" && i + 1 < argc) {
      options.mTestKeybind = static_cast<UINT>(std::clamp(_wtoi(argv[++i]), 0, 0xFE));
    } else if (arg == L"--warp") {
      options.mAllowWarp = true;
    } else if (arg == L"--settings") {
      options.mOpenSettings = true;
    } else if (arg == L"--settings-keybinds") {
      options.mSettingsKeybinds = true;
    } else if (arg == L"--settings-hands") {
      options.mSettingsHands = true;
    } else if (arg == L"--settings-profiles") {
      options.mSettingsProfiles = true;
    } else if (arg == L"--settings-general") {
      options.mSettingsGeneral = true;
    } else if (arg == L"--settings-cycle-tabs") {
      options.mSettingsCycleTabs = true;
    } else if (arg == L"--settings-diagnostics") {
      options.mSettingsDiagnostics = true;
    } else if (arg == L"--save-debug-info" && i + 1 < argc) {
      options.mDebugInfoZip = argv[++i];
    } else if (arg == L"--show-keyboard") {
      options.mShowKeyboard = true;
    } else if (arg == L"--show-quick-menu") {
      options.mShowQuickMenu = true;
    } else if (arg == L"--test-recenter-after" && i + 1 < argc) {
      options.mTestRecenterAfterSeconds = std::clamp(std::wcstod(argv[++i], nullptr), 0.0, 600.0);
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
  // First, so a crash anywhere after leaves a report (TH-10)
  if (const auto dumps = Product::CrashDumpsFolder(); !dumps.empty()) {
    CrashHandler::Install(dumps, L"OpenDesktopVR");
  }
  // Window rectangles and capture crops in physical pixels (doc 04 "DPI")
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto options = ParseOptions();

  // Single instance (doc 02)
  const auto mutexName = SharedObjectName(L"Host", SharedAbi::AbiVersion);
  HANDLE single = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;// already running: its tray icon is the way in
  }

  // The Diagnostics tab's log viewer (LG-3): from the first line on
  Log::LogRing logRing;
  Log::Config logConfig;
  if (const auto folder = Product::LogsFolder(); !folder.empty()) {
    logConfig.mFile = folder / L"host.log";
  }
  Log::Start(logConfig);
  Log::Info("Build: {}", Product::BuildDescription());

  // Elevation and UAC are the top cause of support requests (CMP-8)
  const auto uac = ProcessUtil::UacEnabled();
  Log::Info(
    "Host process: {}; token: {}; UAC: {}",
    ProcessUtil::ElevationName(ProcessUtil::CurrentProcessElevation()),
    ProcessUtil::CurrentElevationType(),
    uac ? (*uac ? "on" : "off") : "unknown");
  // Doc 09 "Environment checks at startup": explained, never blocking
  const auto environment = Diagnostics::CheckEnvironment();
  for (const auto& fact: environment.mFacts) {
    Log::Info("{}", fact);
  }
  for (const auto& problem: environment.mProblems) {
    Log::Warning("{}", problem);
  }

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
    // Keeps captured browsers updating while the game covers them (CT-3).
    // Made before the renderer, whose capture sources hold its leases.
    AppHelper::AppHelperService appHelper;
    // What the keyboard panel draws: changed by the keyboard service, drawn
    // by the renderer (ADR-036)
    const auto keyboardModel = std::make_shared<VirtualKeyboard::KeyboardModel>();
    const auto quickMenuModel = std::make_shared<Host::QuickMenuModel>();
    // What each kind of panel shows
    const bool staticPanels = options.mStaticPanels;
    const bool resizingPanels = options.mResizingPanels;
    Host::RenderLoop renderer {loopOptions,
      [staticPanels, resizingPanels, &appHelper, keyboardModel, quickMenuModel](const Host::PanelDefinition& definition,
        Render::GpuDevice& gpu, Render::MarkDirty markDirty) -> std::unique_ptr<Render::ContentSource> {
        if (definition.mKind == Host::PanelKind::Keyboard) {
          return std::make_unique<VirtualKeyboard::KeyboardSource>(gpu, keyboardModel, std::move(markDirty));
        }
        if (definition.mKind == Host::PanelKind::QuickMenu) {
          return std::make_unique<Host::QuickMenuSource>(gpu, quickMenuModel, std::move(markDirty));
        }
        if (definition.mKind == Host::PanelKind::Window) {
          Capture::WindowCaptureOptions capture {definition.mWindow, definition.mName};
          capture.mKeepUpdating = [&appHelper, id = definition.mID](HWND window) { return appHelper.KeepPainting(id, window); };
          capture.mInput = [&appHelper, id = definition.mID](HWND window) { return appHelper.Input(id, window); };
          return std::make_unique<Capture::WindowCaptureSource>(gpu, std::move(capture), std::move(markDirty));
        }
        return std::make_unique<Render::TestPatternSource>(
          gpu, Render::TestPatternOptions {
            .mTitle = definition.mName,
            .mAnimated = (definition.mAnimated && !staticPanels) || resizingPanels,
            .mResizing = resizingPanels,
          });
      }};
    // HK-11: never the game. Asked of the renderer, so forgotten before the
    // renderer goes (this guard is destroyed first)
    appHelper.SetGameProcess([&renderer] { return renderer.Stats().mLayerProcessID; });
    struct ForgetGame {
      AppHelper::AppHelperService& mService;
      ~ForgetGame() {
        mService.SetGameProcess({});
      }
    } forgetGame {appHelper};
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
    if (options.mOverlappingPanels && testPanels.empty()) {
      testPanels = Host::OverlappingTestPanels();
    }
    if (options.mTestLook && testPanels.empty()) {
      testPanels = Host::DefaultTestPanels();
      auto& still = testPanels.back();
      still.mBrightness = 0.5f;
      still.mTintEnabled = true;
      still.mTintColor = 0xFF0000;
    }
    // A test instance keeps its panels in memory, unless it has a data
    // folder of its own (OPENDESKTOPVR_DATA_FOLDER: settings-migration-test)
    const bool privateFolder = ProcessUtil::GetEnvironment("OPENDESKTOPVR_DATA_FOLDER").has_value();
    Host::PanelManager panels {renderer, !options.mTestInstance || privateFolder, std::move(testPanels)};
    panels.Load();
    if (options.mTestInstance && !privateFolder && !options.mShowQuickMenu) {
      // Tests that count panels see only theirs: no menu button
      auto menu = panels.QuickMenu();
      menu.mButton = false;
      panels.SetQuickMenu(menu);
    }
    renderer.Start();

    // Hands (doc 05): samples from the layer, touches into the captured
    // windows (doc 06), typing on the keyboard (doc 07). Made after the
    // renderer, whose shared block they read, so they stop first; the hand
    // thread stops before the keyboard and the injector, which then posts
    // any release still held (INJ-4).
    Pointer::Injector injector;
    VirtualKeyboard::ClickSound click;
    Host::KeyboardService keyboard {injector, keyboardModel, [&click](float volume) { click.Play(volume); }};
    Host::InjectingSink injectingSink {injector, &keyboard};
    // The quick menu's pointer goes to it (ADR-041), the rest to the injector
    Host::QuickMenuHands quickMenuHands {quickMenuModel};
    Host::PointerRouter pointerRouter {injectingSink, quickMenuHands, Host::QuickMenuPanelID};
    Host::HandService hands {
      renderer.Tracking(), [&renderer] { return renderer.TouchablePanels(); }, &pointerRouter, {}, &keyboard};
    const auto applyHands = [&] {
      hands.SetSettings(panels.Hands());
      renderer.SetHandsEnabled(panels.Hands().mEnabled);
      auto keyboardSettings = panels.Keyboard();
      keyboardSettings.mEnabled = keyboardSettings.mEnabled && panels.Hands().mEnabled;
      keyboard.SetSettings(keyboardSettings);
    };
    applyHands();
    const auto handSettings = panels.Changed().Subscribe(applyHands);
    // Hiding all panels dismisses the keyboard (doc 09)
    const auto allHidden = panels.AllHidden().Subscribe([&keyboard] { keyboard.Dismiss("all panels were hidden"); });
    if (options.mShowKeyboard && options.mTestInstance) {
      keyboard.Toggle();
    }
    Host::QuickMenuController quickMenu {instance, panels, renderer, quickMenuModel, quickMenuHands};
    quickMenu.SetKeyboardPreview([&keyboard](bool show) {
      if (!show) {
        keyboard.Dismiss("its position page closed");
        return true;
      }
      if (keyboard.IsShown()) {
        return false;// already up: left up afterwards
      }
      keyboard.Toggle();
      return true;
    });
    if (options.mShowQuickMenu && options.mTestInstance) {
      quickMenu.Open("--show-quick-menu");
    }
    hands.Start();

    // Keybinds: the keyboard and game controllers, on their own thread
    Host::KeybindService keybinds {instance, panels};
    keybinds.SetKeyboardToggle([&keyboard] { return keyboard.Toggle(); });
    keybinds.SetRecenter([&renderer] { renderer.Recenter(); });
    keybinds.SetQuickMenuToggle([&quickMenu] { return quickMenu.Toggle("its keybind"); });
    // Tests: the keybind's recenter, at a known time
    std::jthread testRecenter;
    if (options.mTestRecenterAfterSeconds > 0 && options.mTestInstance) {
      const auto after = std::chrono::duration<double>(options.mTestRecenterAfterSeconds);
      testRecenter = std::jthread([&renderer, after](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, stop, after, [] { return false; }) && !stop.stop_requested()) {
          Log::Info("Test: recentering, as the keybind would");
          renderer.Recenter();
        }
      });
    }
    if (options.mTestKeybind != 0 && options.mTestInstance) {
      Keybinds::Slots slots;
      slots[0] = Keybinds::Binding::From({Keybinds::Button {Keybinds::DeviceKind::Keyboard, {}, options.mTestKeybind}});
      panels.SetGlobalKeybinds(Keybinds::ActionKind::ToggleAllPanels, slots);// in memory: a test instance saves nothing
    }
    Host::SettingsWindow settings {instance, renderer, panels, keybinds, appHelper, logRing};
    settings.SetQuickMenuOpener([&quickMenu] { quickMenu.Open("the settings window"); });
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
      if (options.mSettingsKeybinds) {
        settings.ShowKeybindsTab();
      }
      if (options.mSettingsProfiles) {
        settings.ShowProfilesTab();
      }
      if (options.mSettingsGeneral) {
        settings.ShowGeneralTab();
      }
      if (options.mSettingsCycleTabs) {
        settings.CycleTabs();
      }
      if (options.mSettingsHands) {
        settings.ShowHandsTab();
      }
      if (options.mSettingsDiagnostics) {
        settings.ShowDiagnosticsTab();
      }
      settings.Show();
    }
    if (!options.mDebugInfoZip.empty()) {
      // Tests: as "Save debug info" would, without the dialogs
      const auto stats = renderer.Stats();
      Host::Support::BundleRequest request;
      request.mZip = options.mDebugInfoZip;
      request.mSelfTest = Host::Support::FromHost(stats, panels);
      request.mStatus = Host::Support::FormatRows(Host::Support::DescribeGame(stats));
      request.mHelpers = Host::Support::FormatRows(Host::Support::DescribeHelpers(panels, appHelper));
      request.mRecentLog = logRing.Text();
      Host::Support::SaveBundle(std::move(request));
    }
    exitCode = tray.Run();
    hands.Stop();
    keyboard.LogLastStats();
    injectingSink.LogLastStats();
    renderer.Stop();
  });

  Log::Info("Host exiting");
  Log::Stop();
  ReleaseMutex(single);
  CloseHandle(single);
  return exitCode;
}
