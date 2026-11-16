// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Shared memory between apphelper-test and its app (apphelper-test-app): what
// the app's IsWindowVisible says about each window, read by the test.
#pragma once

#include <atomic>
#include <cstdint>

namespace OpenDesktopVR::Testing {

struct AppHelperProbe {
  // Set by the test before the app starts
  uint64_t mCover {0};      // the test's window (another process, top-level)
  uint64_t mCoverChild {0}; // a child of it (not top-level)
  std::atomic<uint32_t> mStop {0};

  // Set by the app
  uint64_t mMainWindow {0};  // class Chrome_WidgetWin_OdvrTest
  uint64_t mSecondWindow {0};// the same thread, the same class
  std::atomic<uint32_t> mRounds {0};// probe rounds done
  // IsWindowVisible's answers, the last round (1 or 0)
  std::atomic<int32_t> mCoverVisible {-1};            // through the exe's import table
  std::atomic<int32_t> mCoverVisibleDelayLoaded {-1}; // through a DLL that delay-loads user32
  std::atomic<int32_t> mCoverChildVisible {-1};
  std::atomic<int32_t> mDesktopVisible {-1};
  std::atomic<int32_t> mOwnVisible {-1};// its own visible window
  std::atomic<int32_t> mOwnHidden {-1}; // its own hidden window
  std::atomic<int32_t> mHelperLoaded {-1};
  // EVENT_OBJECT_LOCATIONCHANGE for its main window (HK-7)
  std::atomic<uint32_t> mLocationChanges {0};
  // Calls made by its busy thread (HK-9 under load)
  std::atomic<uint64_t> mBusyCalls {0};
};

inline constexpr wchar_t AppHelperProbeName[] = L"Local\\OdvrAppHelperTestProbe";

}// namespace OpenDesktopVR::Testing
