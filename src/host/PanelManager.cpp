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
  : mRenderer(renderer), mPersistent(persistent && !Product::SettingsFile().empty()), mTestPanels(std::move(testPanels)) {
}

Settings::DocumentSpec PanelManager::Spec() const {
  return {.mPath = Product::SettingsFile(), .mCurrentVersion = 1};
}

void PanelManager::Load() {
  if (!mPersistent) {
    mPanels = mTestPanels.empty() ? DefaultTestPanels() : mTestPanels;
    Push();
    return;
  }
  auto loaded = mStore.Load(Spec());
  mDocument = std::move(loaded.mDocument);
  std::size_t skipped = 0;
  if (auto panels = PanelConfig::Read(mDocument, &skipped)) {
    mPanels = std::move(*panels);
    if (skipped > 0) {
      Log::Warning("Settings: {} panel entr{} couldn't be read and {} skipped (kept in the file)", skipped,
        skipped == 1 ? "y" : "ies", skipped == 1 ? "was" : "were");
    }
  } else {
    // A fresh install: the test panels, saved so the user can see the format
    mPanels = DefaultTestPanels();
    PanelConfig::Write(mDocument, mPanels);
    mStore.Save(Spec().mPath, mDocument);
  }
  Log::Info("Panels: {} loaded", mPanels.size());
  Push();
}

void PanelManager::Refresh() {
  if (!mPersistent) {
    return;
  }
  auto loaded = mStore.Load(Spec());
  mDocument = std::move(loaded.mDocument);
  if (auto panels = PanelConfig::Read(mDocument)) {
    mPanels = std::move(*panels);
    Push();
  }
}

std::optional<PanelDefinition> PanelManager::Find(uint64_t panelID) const {
  for (const auto& panel: mPanels) {
    if (panel.mID == panelID) {
      return panel;
    }
  }
  return std::nullopt;
}

void PanelManager::Edit(const std::function<void(std::vector<PanelDefinition>&)>& edit, bool reread) {
  if (!mPersistent) {
    edit(mPanels);
    Push();
    return;
  }
  if (reread) {
    // Another program may have changed other sections
    auto loaded = mStore.Load(Spec());
    mDocument = std::move(loaded.mDocument);
    if (auto panels = PanelConfig::Read(mDocument)) {
      mPanels = std::move(*panels);
    }
  }
  edit(mPanels);
  PanelConfig::Write(mDocument, mPanels);
  mStore.Save(Spec().mPath, mDocument);// debounced (ST-1)
  Push();
}

uint64_t PanelManager::AddWindow(const WindowUtil::WindowInfo& window, bool reread) {
  uint64_t id = 0;
  Edit([&](std::vector<PanelDefinition>& panels) {
    const auto windows = std::ranges::count_if(panels, [](const auto& p) { return p.mKind == PanelKind::Window; });
    auto panel = NewWindowPanel(window, static_cast<std::size_t>(windows));
    id = panel.mID;
    Log::Info("Panels: added '{}'", StringUtil::Narrow(panel.mName));
    panels.push_back(std::move(panel));
  }, reread);
  return id;
}

void PanelManager::Remove(uint64_t panelID, bool reread) {
  Edit([&](std::vector<PanelDefinition>& panels) {
    std::erase_if(panels, [&](const PanelDefinition& p) {
      if (p.mID == panelID) {
        Log::Info("Panels: removed '{}'", StringUtil::Narrow(p.mName));
        return true;
      }
      return false;
    });
  }, reread);
}

void PanelManager::Update(const PanelDefinition& panel) {
  Edit([&](std::vector<PanelDefinition>& panels) {
    for (auto& existing: panels) {
      if (existing.mID == panel.mID) {
        existing = panel;
        return;
      }
    }
  }, false);
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
