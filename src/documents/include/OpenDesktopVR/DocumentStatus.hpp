// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// What each document panel's source knows (M8a): its page count once read,
// and why it can't show, if it can't. Written by the sources' threads, read
// by the UI thread (the settings window, the quick menu, page keybinds).
// The page a panel shows is in its definition, not here: that's the one
// the user chose, and it's saved.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace OpenDesktopVR::Documents {

enum class DocumentState { Loading, Ready, Failed };

struct DocumentStatus {
  DocumentState mState {DocumentState::Loading};
  uint32_t mPageCount {0};// 0 until read
  std::string mProblem;// why it failed, in the user's words
  bool operator==(const DocumentStatus&) const = default;
};

class StatusBoard {
 public:
  // Called after a change, on the thread that made it: it must only post
  // to the UI thread, and never call back in. Setting another (or none)
  // waits for a call in progress, so what it refers to may go afterwards.
  void SetOnChange(std::function<void()> onChange);

  void Set(uint64_t panelID, const DocumentStatus& status);
  void Remove(uint64_t panelID);
  std::optional<DocumentStatus> Get(uint64_t panelID) const;

 private:
  void Notify();

  mutable std::mutex mMutex;
  std::map<uint64_t, DocumentStatus> mStatus;
  // Held while the callback runs (it only posts), apart from mMutex
  std::mutex mCallMutex;
  std::function<void()> mOnChange;
};

// A page to show, from `wanted`, within a document of `pageCount` pages
// (0: not read yet, so taken as it is)
uint32_t ClampPage(uint32_t wanted, uint32_t pageCount) noexcept;
// `step` pages on from `current`, kept within the document (unknown count:
// never below the first)
uint32_t StepPage(uint32_t current, int step, uint32_t pageCount) noexcept;

}// namespace OpenDesktopVR::Documents
