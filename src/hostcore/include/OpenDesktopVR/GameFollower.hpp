// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Which game's layer is attached, for per-game profiles (ADR-038), from
// the render stats' layer process ID, about once a second.
//
// That ID stays after the game exits (the layer can't clear it as it goes),
// so a game that has ended is remembered and never followed again, and a
// process already gone when first seen isn't followed at all. Test round
// 15: the ended game was followed again every second, switching profiles
// back and forth.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace OpenDesktopVR::Host {

class GameFollower {
 public:
  // Whether a process is still running; the default asks Windows
  using IsRunning = std::function<bool(uint32_t processID)>;

  struct Change {
    enum class Kind { Started, Ended };
    Kind mKind {Kind::Started};
    std::wstring mExecutable;
  };

  explicit GameFollower(IsRunning isRunning = {});

  // The layer's process ID now (0: none) and its executable
  std::optional<Change> Update(uint32_t processID, const std::wstring& executable);

  // The game followed now ("" while none)
  const std::wstring& Executable() const noexcept {
    return mExecutable;
  }

 private:
  IsRunning mIsRunning;
  uint32_t mProcessID {0};
  std::wstring mExecutable;
  uint32_t mEnded {0};// the last game that ended: its ID lingers in the stats
};

}// namespace OpenDesktopVR::Host
