// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/DocumentStatus.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <algorithm>

namespace OpenDesktopVR::Documents {

void StatusBoard::SetOnChange(std::function<void()> onChange) {
  std::scoped_lock lock(mCallMutex);
  mOnChange = std::move(onChange);
}

void StatusBoard::Notify() {
  // Not under mMutex: readers never wait on the callback
  std::scoped_lock lock(mCallMutex);
  if (mOnChange) {
    ThreadUtil::CatchAll("document status change", mOnChange);
  }
}

void StatusBoard::Set(uint64_t panelID, const DocumentStatus& status) {
  {
    std::scoped_lock lock(mMutex);
    auto& entry = mStatus[panelID];
    if (entry == status) {
      return;
    }
    entry = status;
  }
  Notify();
}

void StatusBoard::Remove(uint64_t panelID) {
  {
    std::scoped_lock lock(mMutex);
    if (mStatus.erase(panelID) == 0) {
      return;
    }
  }
  Notify();
}

std::optional<DocumentStatus> StatusBoard::Get(uint64_t panelID) const {
  std::scoped_lock lock(mMutex);
  if (const auto found = mStatus.find(panelID); found != mStatus.end()) {
    return found->second;
  }
  return std::nullopt;
}

uint32_t ClampPage(uint32_t wanted, uint32_t pageCount) noexcept {
  return pageCount == 0 ? wanted : std::min(wanted, pageCount - 1);
}

uint32_t StepPage(uint32_t current, int step, uint32_t pageCount) noexcept {
  const int64_t from = ClampPage(current, pageCount);
  int64_t to = std::max<int64_t>(from + step, 0);
  if (pageCount > 0) {
    to = std::min<int64_t>(to, pageCount - 1);
  }
  return static_cast<uint32_t>(std::min<int64_t>(to, UINT32_MAX));
}

}// namespace OpenDesktopVR::Documents
