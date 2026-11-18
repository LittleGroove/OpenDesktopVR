// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/GameFollower.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>

#include <utility>

namespace OpenDesktopVR::Host {

GameFollower::GameFollower(IsRunning isRunning)
  : mIsRunning(isRunning ? std::move(isRunning) : IsRunning {[](uint32_t id) { return ProcessUtil::IsRunning(id); }}) {
}

std::optional<GameFollower::Change> GameFollower::Update(uint32_t processID, const std::wstring& executable) {
  // The game followed has ended
  if (mProcessID != 0 && !mIsRunning(mProcessID)) {
    mEnded = mProcessID;
    mProcessID = 0;
    return Change {Change::Kind::Ended, std::exchange(mExecutable, {})};
  }
  if (processID == 0 || processID == mProcessID || processID == mEnded) {
    return std::nullopt;
  }
  if (!mIsRunning(processID)) {
    mEnded = processID;// gone already: only its ID was left behind
    return std::nullopt;
  }
  mProcessID = processID;
  mExecutable = executable;
  return Change {Change::Kind::Started, executable};
}

void ProfileReturn::Restore(std::optional<std::string> remembered, std::string_view active, Clock::time_point now) {
  mBefore.reset();
  mWaitingSince.reset();
  if (remembered && !remembered->empty() && *remembered != active) {
    mBefore = std::move(remembered);
    mWaitingSince = now;
  }
}

void ProfileReturn::Replaced(std::string before) {
  mBefore = std::move(before);
  mWaitingSince.reset();
}

void ProfileReturn::Forget() noexcept {
  mBefore.reset();
  mWaitingSince.reset();
}

void ProfileReturn::ForgetIf(std::string_view id) noexcept {
  if (mBefore == id) {
    Forget();
  }
}

std::optional<std::string> ProfileReturn::GameEnded() noexcept {
  mWaitingSince.reset();
  return std::exchange(mBefore, std::nullopt);
}

std::optional<std::string> ProfileReturn::Tick(bool gameFollowed, Clock::time_point now) noexcept {
  if (!mWaitingSince) {
    return std::nullopt;
  }
  if (gameFollowed) {
    mWaitingSince.reset();// back mid-game: the game ending switches back
    return std::nullopt;
  }
  if (now - *mWaitingSince < Grace) {
    return std::nullopt;
  }
  mWaitingSince.reset();
  return std::exchange(mBefore, std::nullopt);
}

}// namespace OpenDesktopVR::Host
