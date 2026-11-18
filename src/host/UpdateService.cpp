// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include "UpdateService.hpp"

#include "PanelManager.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Http.hpp>
#include <OpenDesktopVR/Installer.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Payload.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/Releases.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>

namespace OpenDesktopVR::Host {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.Updates";
constexpr UINT_PTR TimerID = 1;

// What the worker needs, copied: it never touches the host's objects
struct Job {
  std::string mUrl;
  Install::Version mCurrent;
  bool mBeta {false};
  bool mAllowLocalHttp {false};
  fs::path mFolder;
  std::shared_ptr<std::atomic<int>> mProgress;
};

Update::HttpOptions Options(const Job& job, std::uint64_t maxBytes) {
  Update::HttpOptions options;
  options.mMaxBytes = maxBytes;
  options.mAllowLocalHttp = job.mAllowLocalHttp;
  options.mHeaders = {L"Accept: application/vnd.github+json", L"X-GitHub-Api-Version: 2022-11-28"};
  return options;
}

std::string Describe(const Update::HttpResult& result) {
  if (result.mStatus == 403 || result.mStatus == 429) {
    return "GitHub's limit on checks was reached for now";
  }
  return "GitHub couldn't be reached (" + result.mError + ")";
}

// The file's SHA-256, or empty if it can't be read
std::string FileSha256(const fs::path& file) {
  const auto data = FileUtil::ReadAll(file);
  return data ? Install::Sha256Hex(*data) : std::string {};
}

// Everything but `keep` in the downloads folder
void ClearDownloads(const fs::path& folder, const fs::path& keep) {
  std::error_code error;
  for (const auto& entry: fs::directory_iterator(folder, error)) {
    if (entry.path() != keep) {
      DeleteFileW(entry.path().c_str());
    }
  }
}

UpdateService::Result Check(const Job& job) {
  using State = UpdateService::State;
  const auto list = Update::Get(job.mUrl, Options(job, Update::MaxReleaseListBytes));
  if (!list.mOk) {
    return {State::Failed, Describe(list)};
  }
  std::string why;
  const auto releases = Update::ParseReleases(list.mBody, &why, job.mAllowLocalHttp);
  if (!why.empty()) {
    return {State::Failed, "GitHub's answer wasn't readable: " + why};
  }
  const auto offer = Update::ChooseUpdate(releases, job.mCurrent, job.mBeta);
  if (!offer) {
    ClearDownloads(job.mFolder, {});
    return {State::Waiting, std::format("Up to date: {}.", job.mCurrent.ToString())};
  }
  const auto version = offer->mRelease.mVersion.ToString();
  // Saved under the name computed here, never one from GitHub's answer
  const auto fileName = Update::SetupAssetName(offer->mRelease.mVersion);
  UpdateService::Result ready {State::Ready, "", version, offer->mRelease.mPage, job.mFolder / StringUtil::Widen(fileName)};

  // Its checksum: GitHub's digest, or the .sha256 file beside it
  std::string expected = offer->mSetup.mSha256;
  if (expected.empty() && offer->mChecksumFile) {
    const auto file = Update::Get(offer->mChecksumFile->mUrl, Options(job, Update::MaxChecksumBytes));
    const auto hash = file.mOk ? Update::ChecksumFor(file.mBody, offer->mSetup.mName) : std::nullopt;
    if (!hash) {
      return {State::Failed, std::format("{}'s checksum couldn't be read", version)};
    }
    expected = *hash;
  }
  // Downloaded already (an earlier check, or before a restart)
  if (FileUtil::Exists(ready.mSetup) && FileSha256(ready.mSetup) == expected
      && Install::SetupProgramVersion(ready.mSetup) == version) {
    ClearDownloads(job.mFolder, ready.mSetup);
    return ready;
  }
  FileUtil::CreateFolders(job.mFolder);
  ClearDownloads(job.mFolder, {});
  const fs::path part = ready.mSetup.native() + L".part";
  job.mProgress->store(0);
  const auto download = Update::Download(offer->mSetup.mUrl, part, Options(job, offer->mSetup.mSize),
    [&job](std::uint64_t done, std::uint64_t total) {
      job.mProgress->store(total ? static_cast<int>(done * 100 / total) : 0);
      return true;
    });
  if (!download.mOk) {
    return {State::Failed, std::format("{} couldn't be downloaded: {}", version, download.mError)};
  }
  // Size, hash and the version inside: what the release says it is
  const auto size = FileUtil::Size(part).value_or(0);
  const auto hash = FileSha256(part);
  const auto inside = Install::SetupProgramVersion(part);
  if (size != offer->mSetup.mSize || hash != expected || inside != version) {
    DeleteFileW(part.c_str());
    return {State::Failed, std::format("{}'s download didn't check out ({}), so it was deleted",
                             version, size != offer->mSetup.mSize ? "its size" : hash != expected ? "its checksum" : "the version inside")};
  }
  if (!MoveFileExW(part.c_str(), ready.mSetup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(part.c_str());
    return {State::Failed, std::format("{}'s download couldn't be kept", version)};
  }
  return ready;
}

}// namespace

UpdateService::UpdateService(HINSTANCE instance, PanelManager& panels, std::string updatedFrom)
  : mInstance(instance),
    mPanels(panels),
    mProgress(std::make_shared<std::atomic<int>>(0)) {
  mStatus.mUpdatedFrom = std::move(updatedFrom);
  mCurrent = Install::ParseVersion(Product::Version).value_or(Install::Version {});
  mFolder = Product::DataFolder().empty() ? fs::path {} : Product::DataFolder() / L"updates";
  mReleasesUrl = std::format("https://api.github.com/repos/{}/releases?per_page=30", Product::Repository);
  auto delay = std::chrono::duration_cast<Clock::duration>(StartDelay);

  // Only a copy the setup program installed updates itself; a test instance
  // only against its own fake GitHub
  bool available = false;
  const bool test = ProcessUtil::GetEnvironment("OPENDESKTOPVR_INSTANCE").value_or("") != "";
  if (test) {
    if (const auto url = ProcessUtil::GetEnvironment("OPENDESKTOPVR_UPDATE_URL")) {
      mReleasesUrl = *url;
      mAllowLocalHttp = true;
      available = true;
      if (const auto fake = ProcessUtil::GetEnvironment("OPENDESKTOPVR_FAKE_VERSION")) {
        mCurrent = Install::ParseVersion(*fake).value_or(mCurrent);
      }
      if (const auto ms = ProcessUtil::GetEnvironment("OPENDESKTOPVR_UPDATE_DELAY_MS")) {
        delay = std::chrono::milliseconds(std::clamp(std::atoi(ms->c_str()), 0, 600'000));
      }
    }
  }
  const auto installed = Install::FindInstalled(Install::DefaultLayout());
  std::error_code error;
  const bool isInstalledCopy = installed && !installed->mVersion.empty()
    && fs::equivalent(installed->mFolder, ProcessUtil::ExecutablePath().parent_path(), error);
  if (!test) {
    available = isInstalledCopy;
  }
  available = available && !mFolder.empty();

  if (!mStatus.mUpdatedFrom.empty()) {
    Log::Info("Updates: started after an update from {} to {}", mStatus.mUpdatedFrom, mCurrent.ToString());
  }
  if (!available) {
    mStatus.mState = State::Unavailable;
    mStatus.mText = test ? "Off in a test instance."
                         : std::format("This copy wasn't installed by OpenDesktopVR-Setup.exe, so it doesn't update "
                                       "itself. New versions: github.com/{}/releases",
                             Product::Repository);
    return;
  }
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  mWindow = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
  if (!mWindow) {
    mStatus.mState = State::Unavailable;
    mStatus.mText = "Couldn't start.";
    return;
  }
  SetTimer(mWindow, TimerID, 1000, nullptr);
  mNextCheck = Clock::now() + delay;
  Set(mPanels.Updates().mCheck ? State::Waiting : State::Off,
    mPanels.Updates().mCheck ? std::format("{}: checks in a moment.", mCurrent.ToString())
                             : std::format("{}. Checking for new versions is off.", mCurrent.ToString()));
}

UpdateService::~UpdateService() {
  if (mWindow) {
    KillTimer(mWindow, TimerID);
    DestroyWindow(mWindow);
  }
  UnregisterClassW(WindowClass, mInstance);
  if (mSetupProcess) {
    CloseHandle(mSetupProcess);
  }
}

LRESULT CALLBACK UpdateService::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = reinterpret_cast<UpdateService*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self && message == WM_TIMER && wParam == TimerID) {
    ThreadUtil::CatchAll("updates", [self] { self->OnTimer(); });
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

bool UpdateService::CanCheck() const noexcept {
  return mWindow && !mTask.Started() && mStatus.mState != State::Installing;
}

void UpdateService::Set(State state, std::string text) {
  mStatus.mState = state;
  mStatus.mText = std::move(text);
  if (mOnChange) {
    mOnChange();
  }
}

void UpdateService::CheckNow() {
  if (CanCheck()) {
    StartCheck("asked");
  }
}

void UpdateService::StartCheck(const char* why) {
  Job job {mReleasesUrl, mCurrent, mPanels.Updates().mBeta, mAllowLocalHttp, mFolder, mProgress};
  Log::Info("Updates: checking ({}; {})", why, job.mBeta ? "releases and pre-releases" : "releases");
  try {
    mTask = ThreadUtil::BackgroundTask<Result>::Start(L"update check", [job] { return Check(job); });
  } catch (...) {
    mNextCheck = Clock::now() + RetryAfterFailure;
    Set(State::Failed, "Couldn't start a check.");
    return;
  }
  mStatus.mPercent = 0;
  Set(State::Checking, "Checking for a new version...");
}

void UpdateService::OnTimer() {
  const auto now = Clock::now();
  if (mTask.Started()) {
    if (mTask.Running()) {
      if (const int percent = mProgress->load(); percent != mStatus.mPercent && percent > 0) {
        mStatus.mPercent = percent;
        Set(State::Downloading, std::format("Downloading a new version: {}%", percent));
      }
      return;
    }
    const auto result = mTask.Take().value_or(Result {State::Failed, "The check stopped unexpectedly."});
    mStatus.mVersion = result.mVersion;
    mStatus.mPage = result.mPage;
    if (result.mState == State::Ready) {
      mReady = result.mSetup;
      mNextCheck = now + CheckEvery;
      Log::Info("Updates: {} is downloaded and checked", result.mVersion);
      Set(State::Ready, std::format("{} is ready to install.", result.mVersion));
    } else if (result.mState == State::Failed) {
      mReady.clear();
      mNextCheck = now + RetryAfterFailure;
      Log::Warning("Updates: {}", result.mText);
      Set(State::Failed, result.mText + " It tries again in an hour.");
    } else {
      mReady.clear();
      mNextCheck = now + CheckEvery;
      Log::Info("Updates: {}", result.mText);
      Set(State::Waiting, result.mText);
    }
  }

  if (mStatus.mState == State::Installing) {
    // The setup program closes this host; if it ends first, it failed
    DWORD code = STILL_ACTIVE;
    if (mSetupProcess && GetExitCodeProcess(mSetupProcess, &code) && code != STILL_ACTIVE) {
      CloseHandle(std::exchange(mSetupProcess, nullptr));
      mNextCheck = now + RetryAfterFailure;
      Log::Warning("Updates: the setup program ended (exit code {}) without updating", code);
      Set(State::Failed, std::format("The update didn't install (code {}). The setup log in the logs folder says why.", code));
    }
    return;
  }

  const auto& settings = mPanels.Updates();
  if (mStatus.mState == State::Ready && settings.mInstallItself) {
    const auto busy = mBusy ? mBusy() : std::string {};
    if (busy.empty()) {
      Install("by itself: no game running, the settings window closed");
      return;
    }
  }
  if (!settings.mCheck) {
    if (mStatus.mState == State::Waiting) {
      Set(State::Off, std::format("{}. Checking for new versions is off.", mCurrent.ToString()));
    }
    return;
  }
  if (mStatus.mState == State::Off) {
    Set(State::Waiting, std::format("{}: checks in a moment.", mCurrent.ToString()));
    mNextCheck = std::min(mNextCheck, now + std::chrono::seconds(5));
  }
  if (now >= mNextCheck && mStatus.mState != State::Ready) {
    StartCheck("now and then");
  }
}

void UpdateService::InstallNow() {
  if (mStatus.mState == State::Ready) {
    Install("asked");
  }
}

void UpdateService::Install(const char* why) {
  std::wstring line = L"\"" + mReady.wstring() + L"\" --silent";
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (mReady.empty() || !FileUtil::Exists(mReady)
      || !CreateProcessW(mReady.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, mReady.parent_path().c_str(),
        &startup, &info)) {
    Log::Warning("Updates: couldn't start the setup program (error {})", GetLastError());
    mNextCheck = Clock::now() + RetryAfterFailure;
    mReady.clear();
    Set(State::Failed, "The setup program couldn't be started.");
    return;
  }
  CloseHandle(info.hThread);
  mSetupProcess = info.hProcess;
  mInstallStarted = Clock::now();
  Log::Info("Updates: installing {} ({}); the setup program closes this and starts the new one", mStatus.mVersion, why);
  // Pending settings are saved now, as well as when the setup program asks
  mPanels.FlushSettings();
  Set(State::Installing, std::format("Installing {}: Open Desktop VR closes and starts again...", mStatus.mVersion));
}

}// namespace OpenDesktopVR::Host
