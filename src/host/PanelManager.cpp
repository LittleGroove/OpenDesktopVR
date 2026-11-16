// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include "PanelManager.hpp"

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PanelConfig.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>
#include <cmath>

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
  mToggleAllKeybinds = PanelConfig::ReadToggleAllKeybinds(mDocument);
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
  Reread();
  Push();
}

void PanelManager::Reread() {
  auto loaded = mStore.Load(Spec());
  mDocument = std::move(loaded.mDocument);
  mToggleAllKeybinds = PanelConfig::ReadToggleAllKeybinds(mDocument);
  if (auto panels = PanelConfig::Read(mDocument)) {
    mPanels = std::move(*panels);
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
    Reread();// another program may have changed other sections
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

bool PanelManager::ToggleAll() {
  auto result = Visibility::ToggleAll(mPanels, mHidden);
  mHidden = std::move(result.mHidden);
  if (result.mEnableAll) {
    // Nothing was even enabled: enable them all (stored), so the toggle
    // never seems to do nothing
    Edit([](std::vector<PanelDefinition>& panels) {
      for (auto& panel: panels) {
        panel.mEnabled = true;
      }
    }, false);
  } else {
    Push();
  }
  return AnyShowing();
}

bool PanelManager::TogglePanel(uint64_t panelID) {
  mHidden = Visibility::TogglePanel(mPanels, mHidden, panelID);
  Push();
  const auto panel = Find(panelID);
  return panel && Visibility::IsShowing(*panel, mHidden);
}

std::optional<float> PanelManager::ChangeBrightness(uint64_t panelID, float step) {
  auto panel = Find(panelID);
  if (!panel) {
    return std::nullopt;
  }
  // In 10% steps, kept on the step (so ten presses always get back)
  const float level = std::round((panel->mBrightness + step) * 10.0f) / 10.0f;
  panel->mBrightness = std::clamp(level, PanelDefinition::MinimumLevel, 1.0f);
  Update(*panel);
  return panel->mBrightness;
}

std::optional<bool> PanelManager::ToggleTint(uint64_t panelID) {
  auto panel = Find(panelID);
  if (!panel) {
    return std::nullopt;
  }
  panel->mTintEnabled = !panel->mTintEnabled;
  Update(*panel);
  return panel->mTintEnabled;
}

void PanelManager::SetToggleAllKeybinds(const Keybinds::Slots& slots) {
  mToggleAllKeybinds = slots;
  if (mPersistent) {
    PanelConfig::WriteToggleAllKeybinds(mDocument, mToggleAllKeybinds);
    mStore.Save(Spec().mPath, mDocument);// debounced (ST-1)
  }
  mChanged.Emit();
}

void PanelManager::Push() {
  std::vector<PanelDefinition> enabled;
  for (const auto& panel: mPanels) {
    if (panel.mEnabled) {
      enabled.push_back(panel);
    }
  }
  mHidden = Visibility::Prune(mPanels, mHidden);
  mRenderer.SetPanels(std::move(enabled), mHidden);
  mChanged.Emit();
}

}// namespace OpenDesktopVR::Host
