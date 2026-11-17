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

}// namespace OpenDesktopVR::Host
