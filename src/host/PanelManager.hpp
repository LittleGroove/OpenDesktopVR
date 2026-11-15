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
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>

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

 private:
  void Edit(const std::function<void(std::vector<PanelDefinition>&)>& edit, bool reread);
  void Push();
  Settings::DocumentSpec Spec() const;

  RenderLoop& mRenderer;
  const bool mPersistent;
  Settings::Store mStore;
  JsonUtil::Json mDocument;// the whole settings file, as last read
  std::vector<PanelDefinition> mPanels;
  std::vector<PanelDefinition> mTestPanels;
};

}// namespace OpenDesktopVR::Host
