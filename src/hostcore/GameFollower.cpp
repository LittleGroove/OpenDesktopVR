// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/GameFollower.hpp>

#include <Windows.h>

#include <utility>

namespace OpenDesktopVR::Host {

namespace {

bool ProcessIsRunning(uint32_t processID) {
  const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processID);
  if (!process) {
    return false;
  }
  const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return running;
}

}// namespace

GameFollower::GameFollower(IsRunning isRunning)
  : mIsRunning(isRunning ? std::move(isRunning) : IsRunning {&ProcessIsRunning}) {
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
