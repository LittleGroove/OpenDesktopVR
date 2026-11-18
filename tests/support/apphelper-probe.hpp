// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
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

  // Input mode (M4b): what the app saw while handling a WM_LBUTTONDOWN the
  // test sent, and what its probe thread saw at that moment
  std::atomic<uint32_t> mDowns {0};
  std::atomic<int32_t> mDownCursorX {0};
  std::atomic<int32_t> mDownCursorY {0};
  std::atomic<uint32_t> mDownMessagePos {0};
  std::atomic<int32_t> mDownForeground {-1};// GetForegroundWindow() was its main window
  std::atomic<int32_t> mDownActive {-1};    // GetActiveWindow() was its main window
  std::atomic<int32_t> mOtherThreadCursorFaked {-1};
  std::atomic<int32_t> mOtherThreadForegroundFaked {-1};
  // On WM_APP + 1 (sent with no injection open): GetForegroundWindow() was
  // its main window
  std::atomic<uint32_t> mProbes {0};
  std::atomic<int32_t> mProbeForeground {-1};
  // Z-order among its own windows: WM_APP + 2 puts its second window above
  // its main one (1 if it worked); a click then tries to bring the main one
  // to the top (1 if it came above the second)
  std::atomic<int32_t> mSecondOnTop {-1};
  std::atomic<int32_t> mMainCameUp {-1};

  // Keys (M5a): what the app saw while handling a WM_KEYDOWN the test sent
  std::atomic<uint32_t> mKeyDowns {0};
  std::atomic<int32_t> mKeyShift {-1};     // GetKeyState(VK_SHIFT) held
  std::atomic<int32_t> mKeyControl {-1};   // GetKeyState(VK_CONTROL) held
  std::atomic<int32_t> mKeyCapsLock {-1};  // GetKeyState(VK_CAPITAL) toggled
  std::atomic<int32_t> mKeyStateShift {-1};// GetKeyboardState()'s Shift held
  std::atomic<int32_t> mKeyAsyncShift {-1};// GetAsyncKeyState(VK_SHIFT) held
  // The WM_CHAR already waiting in its queue then (0: none), and whether it
  // carries the key's lParam
  std::atomic<uint32_t> mKeyCharWaiting {0};
  std::atomic<int32_t> mKeyCharLParamMatches {-1};
  // The WM_CHARs it handled, in order, and whether each carried the lParam
  // of the key-down before it
  std::atomic<uint32_t> mChars {0};
  std::atomic<uint32_t> mCharCodes[32] {};
  std::atomic<int32_t> mCharsMatchKey {1};
  // On WM_APP + 3 (sent): GetKeyState for Shift (held) and Caps Lock
  // (toggled)
  std::atomic<uint32_t> mKeyProbes {0};
  std::atomic<int32_t> mProbeShift {-1};
  std::atomic<int32_t> mProbeCapsLock {-1};
};

inline constexpr wchar_t AppHelperProbeName[] = L"Local\\OdvrAppHelperTestProbe";

}// namespace OpenDesktopVR::Testing
