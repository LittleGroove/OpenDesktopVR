// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
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
// ("keyboard"; ADR-036). The keyboard's panel goes to the renderer first
// (VK-5) but isn't in Panels(): the settings window never lists it (K10).
// Changed() fires after every change, on the UI thread; AllHidden() when
// panels are all hidden (which dismisses the keyboard, doc 09).
#pragma once

#include <OpenDesktopVR/Events.hpp>
#include <OpenDesktopVR/HandService.hpp>
#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>
#include <OpenDesktopVR/Visibility.hpp>

#include <functional>
#include <optional>
#include <vector>

namespace OpenDesktopVR::Host {

class PanelManager {
 public:
  // `persistent` false: a test instance; `testPanels` (or the built-in
  // test panels), never saved
  PanelManager(RenderLoop& renderer, bool persistent, std::vector<PanelDefinition> testPanels = {});

  void Load();
  // Re-reads the file (other writers), keeping panels in memory if it fails
  void Refresh();

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
  Settings::DocumentSpec Spec() const;

  RenderLoop& mRenderer;
  const bool mPersistent;
  Settings::Store mStore;
  JsonUtil::Json mDocument;// the whole settings file, as last read
  std::vector<PanelDefinition> mPanels;
  std::vector<PanelDefinition> mTestPanels;
  Visibility::Hidden mHidden;
  std::array<Keybinds::Slots, Keybinds::GlobalActions.size()> mGlobalKeybinds {};
  HandSettings mHands;
  KeyboardSettings mKeyboard;
  Events::Event<> mChanged;
  Events::Event<> mAllHidden;
};

}// namespace OpenDesktopVR::Host
