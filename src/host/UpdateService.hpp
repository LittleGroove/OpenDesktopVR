// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Updates from the project's GitHub Releases (ADR-045), for a copy the setup
// program installed (ADR-043):
// - it checks 30 s after start, then every 6 hours (an hour after a
//   failure), or when asked: releases only, or pre-releases too (the beta);
// - it downloads the newer setup program into the data folder's "updates",
//   and checks its size, its SHA-256 (GitHub's digest, or a .sha256 asset)
//   and the version inside it;
// - it installs it by running it silently: the setup program closes this
//   host through the Restart Manager (it saves first), installs, and starts
//   the new one. By itself only when no game runs and the settings window
//   is closed; or when asked.
// The network work runs on a worker; the UI thread only starts it and takes
// its result (ADR-020). Nothing about the user is sent: GitHub sees an
// address and "OpenDesktopVR/<version>".
#pragma once

#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/Version.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace OpenDesktopVR::Host {

class PanelManager;

class UpdateService {
 public:
  enum class State { Unavailable, Off, Waiting, Checking, Downloading, Ready, Installing, Failed };
  struct Status {
    State mState {State::Waiting};
    std::string mText;// for the settings window
    std::string mVersion;// the newer one, when there is one
    std::string mPage;// its release notes
    std::string mUpdatedFrom;// this start followed an update from it
    int mPercent {0};// downloading
  };

  // `updatedFrom`: the version the setup program replaced to start this host
  UpdateService(HINSTANCE instance, PanelManager& panels, std::string updatedFrom);
  ~UpdateService();
  UpdateService(const UpdateService&) = delete;
  UpdateService& operator=(const UpdateService&) = delete;

  // Why installing by itself must wait ("" when it needn't)
  void SetBusy(std::function<std::string()> busy) {
    mBusy = std::move(busy);
  }
  // Called on the UI thread when the status changes
  void SetOnChange(std::function<void()> changed) {
    mOnChange = std::move(changed);
  }
  const Status& GetStatus() const noexcept {
    return mStatus;
  }
  bool CanCheck() const noexcept;
  void CheckNow();
  void InstallNow();

  static constexpr std::chrono::seconds StartDelay {30};
  static constexpr std::chrono::hours CheckEvery {6};
  static constexpr std::chrono::hours RetryAfterFailure {1};

  // What a check found, handed from the worker to the UI thread
  struct Result {
    State mState {State::Failed};
    std::string mText;
    std::string mVersion;
    std::string mPage;
    std::filesystem::path mSetup;
  };

 private:
  using Clock = std::chrono::steady_clock;
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  void OnTimer();
  void StartCheck(const char* why);
  void Install(const char* why);
  void Set(State state, std::string text);

  HINSTANCE mInstance;
  PanelManager& mPanels;
  HWND mWindow {nullptr};
  Status mStatus;
  std::function<std::string()> mBusy;
  std::function<void()> mOnChange;
  Install::Version mCurrent;
  std::string mReleasesUrl;
  bool mAllowLocalHttp {false};
  std::filesystem::path mFolder;// downloads
  ThreadUtil::BackgroundTask<Result> mTask;
  std::shared_ptr<std::atomic<int>> mProgress;
  Clock::time_point mNextCheck;
  std::filesystem::path mReady;// the checked setup program
  HANDLE mSetupProcess {nullptr};
  Clock::time_point mInstallStarted;
};

}// namespace OpenDesktopVR::Host
