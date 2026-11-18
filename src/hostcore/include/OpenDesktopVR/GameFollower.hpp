// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
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

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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

// The profile a game's own profile replaced, to go back to when the game
// ends (ADR-038). The host keeps it in settings.json, so it outlives a
// restart: the setup program restarts the host mid-game when it updates
// (M7a, test round 23), and a host may start after the game has gone.
// Restored at start, it waits a grace period for a game's layer: if one
// attaches, the game ending switches back as usual; if none does, the game
// has gone, and it's time to go back.
class ProfileReturn {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds Grace {10};

  // At start: what settings.json remembered; dropped if it's the active one
  void Restore(std::optional<std::string> remembered, std::string_view active, Clock::time_point now);
  // A game's profile was switched in, replacing `before`
  void Replaced(std::string before);
  // The user chose a profile: the game ending doesn't switch back
  void Forget() noexcept;
  // The profile `id` was deleted
  void ForgetIf(std::string_view id) noexcept;
  // The game ended: the profile to go back to, if any
  std::optional<std::string> GameEnded() noexcept;
  // About once a second. After a restore, a game seen keeps waiting for it
  // to end; none within the grace: the profile to go back to
  std::optional<std::string> Tick(bool gameFollowed, Clock::time_point now) noexcept;

  const std::optional<std::string>& Remembered() const noexcept {
    return mBefore;
  }

 private:
  std::optional<std::string> mBefore;
  std::optional<Clock::time_point> mWaitingSince;// restored; no game seen yet
};

}// namespace OpenDesktopVR::Host
