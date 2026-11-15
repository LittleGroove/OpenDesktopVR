// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Open Desktop VR host app (doc 02): a tray app, single instance, that
// renders panels and shares them with the layer in each game.
//
//   OpenDesktopVR.exe [--no-register] [--exit-after <s>] [--fence-delay-ms <n>] [--static-panels] [--warp]
//
// --no-register      don't register the layer (tests, side-by-side builds)
// --exit-after <s>   quit after this many seconds (tests)
// --fence-delay-ms   test only: announce frames before their GPU work is
//                    signalled, to exercise the layer's bounded staleness
// --static-panels    test panels that never change (the idle bench gate)
// --warp             allow software rendering if there's no GPU (CI)

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
    } else if (arg == L"--static-panels") {
      options.mStaticPanels = true;
    } else if (arg == L"--warp") {
      options.mAllowWarp = true;
    }
  }
  LocalFree(argv);
  // A test instance never touches the real registration
  if (const auto instance = ProcessUtil::GetEnvironment("OPENDESKTOPVR_INSTANCE"); instance && !instance->empty()) {
    options.mRegister = false;
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
    Host::RenderLoop renderer {loopOptions};
    for (const auto& definition: Host::DefaultTestPanels()) {
      const bool animated = !options.mStaticPanels && definition.mID == Host::DefaultTestPanels().front().mID;
      renderer.AddPanel(definition, [definition, animated](Render::GpuDevice& gpu) {
        return std::make_unique<Render::TestPatternSource>(
          gpu, Render::TestPatternOptions {.mTitle = definition.mName, .mAnimated = animated});
      });
    }
    renderer.Start();

    Host::TrayIcon tray {instance, renderer};
    if (options.mExitAfterSeconds > 0) {
      tray.QuitAfter(options.mExitAfterSeconds);
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
