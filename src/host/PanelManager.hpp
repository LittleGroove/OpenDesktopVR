// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The panel list on the UI side: loads it from settings, applies edits
// (add a window, remove a panel), saves them through the debounced store,
// and hands the enabled panels to the renderer. Each edit re-reads the file
// first, so other writers' changes (odvr-layer's "layer" section) survive.
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>

#include <functional>
#include <vector>

namespace OpenDesktopVR::Host {

class PanelManager {
 public:
  // `persistent` false: a test instance; `testPanels` (or the built-in
  // test panels), never saved
  PanelManager(RenderLoop& renderer, bool persistent, std::vector<PanelDefinition> testPanels = {});

  void Load();
  const std::vector<PanelDefinition>& Panels() const noexcept {
    return mPanels;
  }

  void AddWindow(const WindowUtil::WindowInfo& window);
  void Remove(uint64_t panelID);

 private:
  void Edit(const std::function<void(std::vector<PanelDefinition>&)>& edit);
  void Push();
  Settings::DocumentSpec Spec() const;

  RenderLoop& mRenderer;
  const bool mPersistent;
  Settings::Store mStore;
  std::vector<PanelDefinition> mPanels;
  std::vector<PanelDefinition> mTestPanels;
};

}// namespace OpenDesktopVR::Host
