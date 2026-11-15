// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "PanelManager.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PanelConfig.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>

namespace OpenDesktopVR::Host {

PanelManager::PanelManager(RenderLoop& renderer, bool persistent, std::vector<PanelDefinition> testPanels)
  : mRenderer(renderer), mPersistent(persistent), mTestPanels(std::move(testPanels)) {
}

Settings::DocumentSpec PanelManager::Spec() const {
  return {.mPath = Product::SettingsFile(), .mCurrentVersion = 1};
}

void PanelManager::Load() {
  if (!mPersistent || Product::SettingsFile().empty()) {
    mPanels = mTestPanels.empty() ? DefaultTestPanels() : mTestPanels;
    Push();
    return;
  }
  auto loaded = mStore.Load(Spec());
  std::size_t skipped = 0;
  if (auto panels = PanelConfig::Read(loaded.mDocument, &skipped)) {
    mPanels = std::move(*panels);
    if (skipped > 0) {
      Log::Warning("Settings: {} panel entr{} couldn't be read and {} skipped (kept in the file)", skipped,
        skipped == 1 ? "y" : "ies", skipped == 1 ? "was" : "were");
    }
  } else {
    // A fresh install: the test panels, saved so the user can see the format
    mPanels = DefaultTestPanels();
    PanelConfig::Write(loaded.mDocument, mPanels);
    mStore.Save(Spec().mPath, std::move(loaded.mDocument));
  }
  Log::Info("Panels: {} loaded", mPanels.size());
  Push();
}

void PanelManager::Edit(const std::function<void(std::vector<PanelDefinition>&)>& edit) {
  if (!mPersistent) {
    edit(mPanels);
    Push();
    return;
  }
  // Re-read first: another program may have changed other sections
  auto loaded = mStore.Load(Spec());
  auto panels = PanelConfig::Read(loaded.mDocument).value_or(mPanels);
  edit(panels);
  PanelConfig::Write(loaded.mDocument, panels);
  mStore.Save(Spec().mPath, std::move(loaded.mDocument));
  mPanels = std::move(panels);
  Push();
}

void PanelManager::AddWindow(const WindowUtil::WindowInfo& window) {
  Edit([&](std::vector<PanelDefinition>& panels) {
    const auto windows = std::ranges::count_if(panels, [](const auto& p) { return p.mKind == PanelKind::Window; });
    auto panel = NewWindowPanel(window, static_cast<std::size_t>(windows));
    Log::Info("Panels: added '{}'", StringUtil::Narrow(panel.mName));
    panels.push_back(std::move(panel));
  });
}

void PanelManager::Remove(uint64_t panelID) {
  Edit([&](std::vector<PanelDefinition>& panels) {
    std::erase_if(panels, [&](const PanelDefinition& p) {
      if (p.mID == panelID) {
        Log::Info("Panels: removed '{}'", StringUtil::Narrow(p.mName));
        return true;
      }
      return false;
    });
  });
}

void PanelManager::Push() {
  std::vector<PanelDefinition> enabled;
  for (const auto& panel: mPanels) {
    if (panel.mEnabled) {
      enabled.push_back(panel);
    }
  }
  mRenderer.SetPanels(std::move(enabled));
}

}// namespace OpenDesktopVR::Host
