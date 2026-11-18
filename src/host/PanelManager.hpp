// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The panel list on the UI side: loads it from settings, applies edits,
// saves them through the debounced store (ST-1), and hands the enabled
// panels to the renderer.
//
// Two kinds of edit:
// - menu actions (add a window, remove a panel) re-read the file first, so
//   other writers' changes (odvr-layer's "layer" section) survive;
// - settings-window edits (every slider step) work on the copy in memory,
//   so 300 slider steps are 300 cheap updates and about one disk write.
//   Opening the settings window re-reads the file once.
//
// It also holds what isn't stored: which panels are hidden for now (PN-6,
// Visibility.hpp); and some global settings: the global keybinds' slots
// (stored in "keybinds"), the hand settings ("hands") and the keyboard's
// ("keyboard"; ADR-036) and the quick menu's ("quickMenu"; ADR-041). The
// keyboard's panel goes to the renderer first (VK-5), then the quick
// menu's; neither is in Panels(): the settings window never lists them
// (K10) and hiding all panels never hides them.
// Changed() fires after every change, on the UI thread; AllHidden() when
// panels are all hidden (which dismisses the keyboard, doc 09).
//
// Profiles (ADR-038): the panel list is the active profile's, kept in
// profiles\<id>.json; settings.json holds which one is active. Switching
// flushes pending writes first (ST-3) and shows every panel again. A game
// whose executable a profile claims switches to it when its layer attaches,
// and back when it ends, unless the user switched by hand meanwhile.
#pragma once

#include <OpenDesktopVR/Events.hpp>
#include <OpenDesktopVR/GameFollower.hpp>
#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/ProfileLibrary.hpp>
#include <OpenDesktopVR/QuickMenuConfig.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>
#include <OpenDesktopVR/Visibility.hpp>

#include <Windows.h>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace OpenDesktopVR::Host {

class PanelManager {
 public:
  // `persistent` false: a test instance; `testPanels` (or the built-in
  // test panels), never saved
  PanelManager(RenderLoop& renderer, bool persistent, std::vector<PanelDefinition> testPanels = {});
  PanelManager(const PanelManager&) = delete;
  PanelManager& operator=(const PanelManager&) = delete;

  void Load();
  // This start found no profiles and nothing the host writes in settings
  // (the installer's layer settings may be there): a fresh install (M6f:
  // the settings window opens by itself, with a welcome)
  bool IsFirstRun() const noexcept {
    return mFirstRun;
  }
  // Re-reads the file (other writers), keeping panels in memory if it fails
  void Refresh();
  // Writes pending (debounced) settings and profiles now: before Windows
  // ends the process (sign-out, the setup program closing it)
  void FlushSettings();

  const std::vector<PanelDefinition>& Panels() const noexcept {
    return mPanels;
  }
  std::optional<PanelDefinition> Find(uint64_t panelID) const;

  // Re-read the file first for tray-menu actions; the settings window
  // re-read it when it opened. AddWindow returns the new panel's ID.
  uint64_t AddWindow(const WindowUtil::WindowInfo& window, bool reread = true);
  void Remove(uint64_t panelID, bool reread = true);

  // Settings-window edits: replace the panel with this ID (ST-6: only it)
  void Update(const PanelDefinition& panel);

  // Showing and hiding (PN-6): hides are runtime only
  bool AnyShowing() const noexcept {
    return Visibility::AnyShowing(mPanels, mHidden);
  }
  bool IsHidden(uint64_t panelID) const noexcept {
    return mHidden.contains(panelID);
  }
  // True if panels are now showing
  bool ToggleAll();
  // True if the panel is now showing
  bool TogglePanel(uint64_t panelID);
  // Shows or hides one panel, or every panel, outright (automation); true
  // if anything changed. A panel turned off in the settings stays off.
  bool SetPanelShown(uint64_t panelID, bool shown);
  bool SetAllShown(bool shown);

  // Keybind actions on one panel's look (stored); the new value
  std::optional<float> ChangeBrightness(uint64_t panelID, float step);
  std::optional<bool> ToggleTint(uint64_t panelID);

  // The global keybinds (Keybinds::GlobalActions)
  const Keybinds::Slots& GlobalKeybinds(Keybinds::ActionKind kind) const noexcept;
  void SetGlobalKeybinds(Keybinds::ActionKind kind, const Keybinds::Slots& slots);

  // Tapping and scrolling with the hands (doc 05)
  const HandSettings& Hands() const noexcept {
    return mHands;
  }
  void SetHands(const HandSettings& hands);

  // The virtual keyboard (doc 07)
  const KeyboardSettings& Keyboard() const noexcept {
    return mKeyboard;
  }
  void SetKeyboard(const KeyboardSettings& keyboard);

  // The quick menu in VR (doc 09 "In-VR UI"); its panel is there while
  // the hands are on
  const QuickMenuSettings& QuickMenu() const noexcept {
    return mQuickMenu;
  }
  void SetQuickMenu(const QuickMenuSettings& menu);

  // PN-12: panels update at most this often ("performance" in settings)
  double MaxUpdatesPerSecond() const noexcept {
    return mMaxUpdatesPerSecond;
  }
  void SetMaxUpdatesPerSecond(double perSecond);
  static constexpr std::array<int, 8> UpdateRates {10, 15, 24, 30, 45, 60, 72, 90};

  // New versions from GitHub (ADR-045; "updates" in settings)
  struct UpdateSettings {
    bool mCheck {true};// look for them now and then
    bool mInstallItself {true};// when no game runs and the settings window is closed
    bool mBeta {false};// pre-releases too
    bool operator==(const UpdateSettings&) const = default;
  };
  const UpdateSettings& Updates() const noexcept {
    return mUpdates;
  }
  void SetUpdates(const UpdateSettings& updates);

  // The layer in games ("layer" in settings, read by the layer when a game
  // starts: VR-8)
  struct GameSettings {
    bool mLayerEnabled {true};
    std::vector<std::wstring> mDisabledGames;
    bool operator==(const GameSettings&) const = default;
  };
  GameSettings Games() const;
  void SetGames(const GameSettings& games);

  // Profiles (ADR-038)
  struct ProfileSummary {
    std::string mID;
    std::wstring mName;
    std::vector<std::wstring> mGames;
    std::size_t mPanelCount {0};
  };
  std::vector<ProfileSummary> Profiles() const;
  const std::string& ActiveProfile() const noexcept {
    return mActiveProfile;
  }
  std::wstring ProfileName(std::string_view id) const;
  // By hand: false if it's already active or unknown
  bool SwitchProfile(std::string_view id, const char* why = "chosen");
  // The next (+1) or previous (-1) profile, wrapping round
  bool StepProfile(int direction);
  // A new profile with a copy of the active one's panels; its ID
  std::string NewProfile(std::wstring name);
  bool RenameProfile(std::string_view id, std::wstring name);
  bool SetProfileGames(std::string_view id, std::vector<std::wstring> games);
  // Never the last one; the active one hands over to another first
  bool DeleteProfile(std::string_view id);
  bool ExportProfile(std::string_view id, const std::filesystem::path& to);
  std::optional<std::string> ImportProfile(const std::filesystem::path& from);
  // The game whose layer is attached (process ID 0: none), about once a
  // second from the UI thread: per-game switching
  void FollowGame(uint32_t processID, const std::wstring& executable);
  // The game followed now ("" while none)
  const std::wstring& AttachedGame() const noexcept {
    return mGame.Executable();
  }

  Events::Event<>& Changed() noexcept {
    return mChanged;
  }
  Events::Event<>& AllHidden() noexcept {
    return mAllHidden;
  }

 private:
  void Edit(const std::function<void(std::vector<PanelDefinition>&)>& edit, bool reread);
  void Reread();
  void ReadGlobals();
  void Push();
  void SaveSettings();
  bool Switch(std::string_view id, const char* why, bool byHand);
  // settings.json's "profileBeforeGame", from mReturn
  void SaveReturn();
  Settings::DocumentSpec Spec() const;

  RenderLoop& mRenderer;
  const bool mPersistent;
  Settings::Store mStore;
  bool mFirstRun {false};
  JsonUtil::Json mDocument;// the whole settings file, as last read
  ProfileLibrary mProfiles;
  std::string mActiveProfile;
  // Per-game switching: the game followed, and the profile to go back to
  // (kept in settings.json across restarts)
  GameFollower mGame;
  ProfileReturn mReturn;
  std::vector<PanelDefinition> mPanels;
  std::vector<PanelDefinition> mTestPanels;
  Visibility::Hidden mHidden;
  std::array<Keybinds::Slots, Keybinds::GlobalActions.size()> mGlobalKeybinds {};
  HandSettings mHands;
  KeyboardSettings mKeyboard;
  QuickMenuSettings mQuickMenu;
  double mMaxUpdatesPerSecond {Panels::DefaultMaxUpdatesPerSecond};
  UpdateSettings mUpdates;
  Events::Event<> mChanged;
  Events::Event<> mAllHidden;
};

}// namespace OpenDesktopVR::Host
