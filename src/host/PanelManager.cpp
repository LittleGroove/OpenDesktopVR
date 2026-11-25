// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include "PanelManager.hpp"

#include <OpenDesktopVR/GlobalSettings.hpp>
#include <OpenDesktopVR/HandConfig.hpp>
#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PanelConfig.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace OpenDesktopVR::Host {

PanelManager::PanelManager(RenderLoop& renderer, bool persistent, std::vector<PanelDefinition> testPanels)
  : mRenderer(renderer),
    mPersistent(persistent && !Product::SettingsFile().empty()),
    mProfiles(mStore, Product::ProfilesFolder()),
    mTestPanels(std::move(testPanels)) {
}

Settings::DocumentSpec PanelManager::Spec() const {
  return Settings::GlobalSettingsSpec();
}

void PanelManager::SaveSettings() {
  if (mPersistent) {
    mStore.Save(Spec().mPath, mDocument);// debounced (ST-1)
  }
}

void PanelManager::Load() {
  if (!mPersistent) {
    // A test instance: one profile, in memory only
    mPanels = mTestPanels.empty() ? DefaultTestPanels() : mTestPanels;
    mActiveProfile = "test";
    Push();
    return;
  }
  auto loaded = mStore.Load(Spec());
  mDocument = std::move(loaded.mDocument);
  ReadGlobals();
  const auto unreadable = mProfiles.Load();
  // A first start: the host has never run here. The installer writes
  // settings.json's "layer" part before it does (odvr-layer test-panel off;
  // round 22), so the file being there isn't enough: what only the host
  // writes is, the active profile (version 1: the panel list), and profiles
  const bool hostRanHere = mDocument.contains("activeProfile") || mDocument.contains("panels");
  mFirstRun = !hostRanHere && mProfiles.All().empty() && unreadable == 0;
  if (mFirstRun) {
    Log::Info("First start: no profiles, and no settings from an earlier start");
  }
  // Version 1's panel list becomes the profile "Default" (ADR-038; the
  // store backed the old file up when it migrated it)
  std::size_t skipped = 0;
  if (auto old = PanelConfig::Read(mDocument, &skipped)) {
    if (mProfiles.All().empty()) {
      const auto id = mProfiles.Create(L"Default", std::move(*old));
      mDocument["activeProfile"] = id;
      mDocument.erase("panels");
      SaveSettings();
      Log::Info("Profiles: the panel list moved to the profile 'Default'{}",
        skipped > 0 ? std::format(" ({} unreadable entries left behind in the backup)", skipped) : std::string {});
    } else {
      Log::Warning("Settings: a panel list in settings.json is ignored: panels are in profiles now (ADR-038)");
    }
  }
  if (mProfiles.All().empty()) {
    // A fresh install (or every profile unreadable): the test panels
    mProfiles.Create(L"Default", DefaultTestPanels());
  }
  mActiveProfile = JsonUtil::GetOr<std::string>(mDocument, "activeProfile", "");
  if (!mProfiles.Find(mActiveProfile)) {
    mActiveProfile = mProfiles.All().front().mID;
    mDocument["activeProfile"] = mActiveProfile;
    SaveSettings();
  }
  mPanels = mProfiles.Find(mActiveProfile)->mPanels;
  Log::Info("Profiles: {} ({} unreadable, set aside); active '{}' with {} panels", mProfiles.All().size(), unreadable,
    StringUtil::Narrow(ProfileName(mActiveProfile)), mPanels.size());
  // Started on a game's profile (an update restarts the host mid-game):
  // the profile it replaced, remembered for when the game ends
  auto remembered = JsonUtil::Get<std::string>(mDocument, "profileBeforeGame");
  if (remembered && !mProfiles.Find(*remembered)) {
    remembered.reset();
  }
  mReturn.Restore(remembered, mActiveProfile, ProfileReturn::Clock::now());
  if (const auto& back = mReturn.Remembered()) {
    Log::Info("Profiles: back to '{}' when the game ends, or in {} s if it isn't running",
      StringUtil::Narrow(ProfileName(*back)), ProfileReturn::Grace.count());
  }
  SaveReturn();
  Push();
}

void PanelManager::SaveReturn() {
  const auto& back = mReturn.Remembered();
  const bool saved = mDocument.contains("profileBeforeGame");
  if (back) {
    if (!saved || JsonUtil::GetOr<std::string>(mDocument, "profileBeforeGame", "") != *back) {
      mDocument["profileBeforeGame"] = *back;
      SaveSettings();
    }
  } else if (saved) {
    mDocument.erase("profileBeforeGame");
    SaveSettings();
  }
}

void PanelManager::FlushSettings() {
  mStore.Flush();
}

void PanelManager::Refresh() {
  if (!mPersistent) {
    return;
  }
  Reread();
  Push();
}

void PanelManager::Reread() {
  // settings.json only: other programs (odvr-layer) edit it; the panels are
  // the active profile's, kept here
  auto loaded = mStore.Load(Spec());
  mDocument = std::move(loaded.mDocument);
  mDocument["activeProfile"] = mActiveProfile;
  ReadGlobals();
}

void PanelManager::ReadGlobals() {
  for (std::size_t i = 0; i < Keybinds::GlobalActions.size(); ++i) {
    mGlobalKeybinds[i] = PanelConfig::ReadGlobalKeybinds(mDocument, Keybinds::GlobalActions[i]);
  }
  mHands = HandConfig::Read(mDocument);
  mKeyboard = KeyboardConfig::Read(mDocument);
  mQuickMenu = QuickMenuConfig::Read(mDocument);
  const auto performance = JsonUtil::GetOr<JsonUtil::Json>(mDocument, "performance", JsonUtil::Json {});
  const double rate = JsonUtil::GetOr<double>(performance, "maxUpdatesPerSecond", Panels::DefaultMaxUpdatesPerSecond);
  mMaxUpdatesPerSecond = std::isfinite(rate) ? std::clamp(rate, 1.0, 120.0) : Panels::DefaultMaxUpdatesPerSecond;
  mRenderer.SetGlobalMaxUpdatesPerSecond(mMaxUpdatesPerSecond);
  const auto updates = JsonUtil::GetOr<JsonUtil::Json>(mDocument, "updates", JsonUtil::Json {});
  mUpdates.mCheck = JsonUtil::GetOr<bool>(updates, "check", true);
  mUpdates.mInstallItself = JsonUtil::GetOr<bool>(updates, "installItself", true);
  mUpdates.mBeta = JsonUtil::GetOr<bool>(updates, "beta", false);
}

void PanelManager::SetUpdates(const UpdateSettings& updates) {
  if (updates == mUpdates) {
    return;
  }
  mUpdates = updates;
  JsonUtil::Json saved = JsonUtil::GetOr<JsonUtil::Json>(mDocument, "updates", JsonUtil::Json::object());
  if (!saved.is_object()) {
    saved = JsonUtil::Json::object();
  }
  saved["check"] = mUpdates.mCheck;
  saved["installItself"] = mUpdates.mInstallItself;
  saved["beta"] = mUpdates.mBeta;
  mDocument["updates"] = std::move(saved);
  SaveSettings();
  Log::Info("Updates: check {}, install by itself {}, beta {}", mUpdates.mCheck ? "on" : "off",
    mUpdates.mInstallItself ? "on" : "off", mUpdates.mBeta ? "on" : "off");
  mChanged.Emit();
}

void PanelManager::SetMaxUpdatesPerSecond(double perSecond) {
  if (!std::isfinite(perSecond) || perSecond == mMaxUpdatesPerSecond) {
    return;
  }
  mMaxUpdatesPerSecond = std::clamp(perSecond, 1.0, 120.0);
  JsonUtil::Json performance = JsonUtil::GetOr<JsonUtil::Json>(mDocument, "performance", JsonUtil::Json::object());
  if (!performance.is_object()) {
    performance = JsonUtil::Json::object();
  }
  performance["maxUpdatesPerSecond"] = mMaxUpdatesPerSecond;
  mDocument["performance"] = std::move(performance);
  SaveSettings();
  mRenderer.SetGlobalMaxUpdatesPerSecond(mMaxUpdatesPerSecond);
  mChanged.Emit();
}

PanelManager::GameSettings PanelManager::Games() const {
  GameSettings games;
  const auto layer = JsonUtil::GetOr<JsonUtil::Json>(mDocument, "layer", JsonUtil::Json {});
  games.mLayerEnabled = JsonUtil::GetOr<bool>(layer, "enabled", true);
  if (const auto list = JsonUtil::Get<JsonUtil::Json>(layer, "disabledGames"); list && list->is_array()) {
    for (const auto& game: *list) {
      if (game.is_string()) {
        games.mDisabledGames.push_back(StringUtil::Widen(game.get<std::string>()));
      }
    }
  }
  return games;
}

void PanelManager::SetGames(const GameSettings& games) {
  if (games == Games()) {
    return;
  }
  // Other fields of the layer's section (the test panel) are kept
  JsonUtil::Json layer = JsonUtil::GetOr<JsonUtil::Json>(mDocument, "layer", JsonUtil::Json::object());
  if (!layer.is_object()) {
    layer = JsonUtil::Json::object();
  }
  layer["enabled"] = games.mLayerEnabled;
  JsonUtil::Json list = JsonUtil::Json::array();
  for (const auto& game: games.mDisabledGames) {
    list.push_back(StringUtil::Narrow(game));
  }
  layer["disabledGames"] = std::move(list);
  mDocument["layer"] = std::move(layer);
  SaveSettings();
  Log::Info("Games: the layer {}; off in {} game{}", games.mLayerEnabled ? "on" : "off", games.mDisabledGames.size(),
    games.mDisabledGames.size() == 1 ? "" : "s");
  mChanged.Emit();
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
  if (const auto* active = mProfiles.Find(mActiveProfile)) {
    auto profile = *active;
    profile.mPanels = mPanels;
    mProfiles.Update(profile);// debounced (ST-1)
  }
  Push();
}

std::vector<PanelManager::ProfileSummary> PanelManager::Profiles() const {
  std::vector<ProfileSummary> list;
  if (!mPersistent) {
    list.push_back({mActiveProfile, L"Test panels", {}, mPanels.size()});
    return list;
  }
  for (const auto& profile: mProfiles.All()) {
    list.push_back({profile.mID, profile.mName, profile.mGames, profile.mPanels.size()});
  }
  return list;
}

std::wstring PanelManager::ProfileName(std::string_view id) const {
  const auto* profile = mProfiles.Find(id);
  return profile ? profile->mName : std::wstring {L"?"};
}

bool PanelManager::Switch(std::string_view id, const char* why, bool byHand) {
  const auto* profile = mProfiles.Find(id);
  if (!mPersistent || !profile || id == mActiveProfile) {
    return false;
  }
  mStore.Flush();// ST-3
  if (byHand) {
    mReturn.Forget();// the user chose: the game ending doesn't switch back
  }
  mActiveProfile = std::string {id};
  mPanels = profile->mPanels;
  mHidden.clear();
  mDocument["activeProfile"] = mActiveProfile;
  SaveSettings();
  SaveReturn();
  Log::Info("Profile: '{}' ({}), {} panels", StringUtil::Narrow(profile->mName), why, mPanels.size());
  Push();
  return true;
}

bool PanelManager::SwitchProfile(std::string_view id, const char* why) {
  return Switch(id, why, true);
}

bool PanelManager::StepProfile(int direction) {
  const auto& all = mProfiles.All();
  if (!mPersistent || all.size() < 2) {
    return false;
  }
  const auto current = std::ranges::find(all, mActiveProfile, &Profile::mID);
  const auto index = current == all.end() ? 0 : static_cast<std::ptrdiff_t>(current - all.begin());
  const auto count = static_cast<std::ptrdiff_t>(all.size());
  const auto next = ((index + (direction < 0 ? -1 : 1)) % count + count) % count;
  return Switch(all[static_cast<std::size_t>(next)].mID, direction < 0 ? "previous profile" : "next profile", true);
}

std::string PanelManager::NewProfile(std::wstring name) {
  if (!mPersistent) {
    return {};
  }
  const auto id = mProfiles.Create(std::move(name), mPanels);
  Log::Info("Profiles: '{}' made, a copy of '{}'", StringUtil::Narrow(ProfileName(id)),
    StringUtil::Narrow(ProfileName(mActiveProfile)));
  mChanged.Emit();
  return id;
}

bool PanelManager::RenameProfile(std::string_view id, std::wstring name) {
  const auto* found = mProfiles.Find(id);
  if (!found || name.empty()) {
    return false;
  }
  auto profile = *found;
  profile.mName = std::move(name);
  mProfiles.Update(profile);
  mChanged.Emit();
  return true;
}

bool PanelManager::SetProfileGames(std::string_view id, std::vector<std::wstring> games) {
  const auto* found = mProfiles.Find(id);
  if (!found) {
    return false;
  }
  auto profile = *found;
  profile.mGames = std::move(games);
  mProfiles.Update(profile);
  mChanged.Emit();
  return true;
}

bool PanelManager::DeleteProfile(std::string_view id) {
  if (!mPersistent || mProfiles.All().size() < 2 || !mProfiles.Find(id)) {
    return false;
  }
  if (id == mActiveProfile) {
    for (const auto& other: mProfiles.All()) {
      if (other.mID != id) {
        Switch(other.mID, "the active profile was deleted", true);
        break;
      }
    }
  }
  const auto name = ProfileName(id);
  if (!mProfiles.Remove(id)) {
    return false;
  }
  mReturn.ForgetIf(id);
  SaveReturn();
  Log::Info("Profiles: '{}' deleted", StringUtil::Narrow(name));
  mChanged.Emit();
  return true;
}

bool PanelManager::ExportProfile(std::string_view id, const std::filesystem::path& to) {
  const bool ok = mPersistent && mProfiles.Export(id, to);
  Log::Info("Profiles: '{}' {}", StringUtil::Narrow(ProfileName(id)), ok ? "exported" : "couldn't be exported");
  return ok;
}

std::optional<std::string> PanelManager::ImportProfile(const std::filesystem::path& from) {
  if (!mPersistent) {
    return std::nullopt;
  }
  auto id = mProfiles.Import(from);
  if (id) {
    Log::Info("Profiles: '{}' imported", StringUtil::Narrow(ProfileName(*id)));
    mChanged.Emit();
  } else {
    Log::Warning("Profiles: that file isn't a profile; nothing imported");
  }
  return id;
}

void PanelManager::FollowGame(uint32_t processID, const std::wstring& executable) {
  const auto change = mGame.Update(processID, executable);
  const auto now = ProfileReturn::Clock::now();
  if (!change) {
    // Started on a game's profile, and no game's layer came in time: the
    // game has gone, so back to the profile it replaced
    if (const auto back = mReturn.Tick(!mGame.Executable().empty(), now)) {
      Switch(*back, "the game it was for isn't running", false);
      SaveReturn();
      mChanged.Emit();
    }
    return;
  }
  if (change->mKind == GameFollower::Change::Kind::Ended) {
    // Back to the profile it replaced, if it switched one in and the user
    // hasn't chosen another since
    Log::Info("Profiles: {} ended", StringUtil::Narrow(change->mExecutable));
    if (const auto back = mReturn.GameEnded()) {
      Switch(*back, "the game ended", false);
    }
    SaveReturn();
    mChanged.Emit();
    return;
  }
  // A game's layer attached: its profile, if one claims it
  Log::Info("Profiles: {} started", StringUtil::Narrow(change->mExecutable));
  mReturn.Tick(true, now);
  mChanged.Emit();
  if (const auto id = mProfiles.ForGame(change->mExecutable); id && *id != mActiveProfile) {
    const auto before = mActiveProfile;
    if (Switch(*id, "the game claims it", false)) {
      mReturn.Replaced(before);
      SaveReturn();
    }
  }
}

namespace {

// Windows and documents share one grid of spots
std::size_t GridPanels(const std::vector<PanelDefinition>& panels) {
  return static_cast<std::size_t>(
    std::ranges::count_if(panels, [](const auto& p) { return p.mKind == PanelKind::Window || p.mKind == PanelKind::Document; }));
}

}// namespace

uint64_t PanelManager::AddDocument(const std::wstring& path, bool reread) {
  uint64_t id = 0;
  Edit([&](std::vector<PanelDefinition>& panels) {
    auto panel = NewDocumentPanel(path, GridPanels(panels));
    id = panel.mID;
    Log::Info("Panels: added the document '{}'", StringUtil::Narrow(panel.mName));
    panels.push_back(std::move(panel));
  }, reread);
  return id;
}

std::optional<Documents::DocumentStatus> PanelManager::DocumentStatusOf(uint64_t panelID) const {
  return mDocuments ? mDocuments->Get(panelID) : std::nullopt;
}

std::optional<uint32_t> PanelManager::StepPage(uint64_t panelID, int step) {
  auto panel = Find(panelID);
  if (!panel || panel->mKind != PanelKind::Document) {
    return std::nullopt;
  }
  const auto status = DocumentStatusOf(panelID);
  const uint32_t count = status && status->mState == Documents::DocumentState::Ready ? status->mPageCount : 0;
  const auto page = Documents::StepPage(panel->mPage, step, count);
  if (page != panel->mPage) {
    panel->mPage = page;
    Update(*panel);
  }
  return page;
}

std::optional<uint32_t> PanelManager::SetPage(uint64_t panelID, uint32_t page) {
  auto panel = Find(panelID);
  if (!panel || panel->mKind != PanelKind::Document) {
    return std::nullopt;
  }
  const auto status = DocumentStatusOf(panelID);
  const uint32_t count = status && status->mState == Documents::DocumentState::Ready ? status->mPageCount : 0;
  page = Documents::ClampPage(page, count);
  if (page != panel->mPage) {
    panel->mPage = page;
    Update(*panel);
  }
  return page;
}

uint64_t PanelManager::AddWindow(const WindowUtil::WindowInfo& window, bool reread) {
  uint64_t id = 0;
  Edit([&](std::vector<PanelDefinition>& panels) {
    auto panel = NewWindowPanel(window, GridPanels(panels));
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
  const bool showing = AnyShowing();
  if (!showing) {
    mAllHidden.Emit();// hiding all dismisses the keyboard (doc 09)
  }
  return showing;
}

bool PanelManager::TogglePanel(uint64_t panelID) {
  mHidden = Visibility::TogglePanel(mPanels, mHidden, panelID);
  Push();
  const auto panel = Find(panelID);
  return panel && Visibility::IsShowing(*panel, mHidden);
}

bool PanelManager::SetPanelShown(uint64_t panelID, bool shown) {
  const auto panel = Find(panelID);
  if (!panel || !panel->mEnabled || mHidden.contains(panelID) != shown) {
    return false;// gone, off, or already so
  }
  if (shown) {
    mHidden.erase(panelID);
  } else {
    mHidden.insert(panelID);
  }
  Push();
  if (!AnyShowing()) {
    mAllHidden.Emit();// hiding all dismisses the keyboard (doc 09)
  }
  return true;
}

bool PanelManager::SetAllShown(bool shown) {
  bool changed = false;
  if (shown) {
    changed = !mHidden.empty();
    mHidden.clear();
  } else {
    for (const auto& panel: mPanels) {
      changed |= panel.mEnabled && mHidden.insert(panel.mID).second;
    }
  }
  if (changed) {
    Push();
    if (!shown) {
      mAllHidden.Emit();
    }
  }
  return changed;
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

const Keybinds::Slots& PanelManager::GlobalKeybinds(Keybinds::ActionKind kind) const noexcept {
  for (std::size_t i = 0; i < Keybinds::GlobalActions.size(); ++i) {
    if (Keybinds::GlobalActions[i] == kind) {
      return mGlobalKeybinds[i];
    }
  }
  static const Keybinds::Slots none {};
  return none;
}

void PanelManager::SetGlobalKeybinds(Keybinds::ActionKind kind, const Keybinds::Slots& slots) {
  for (std::size_t i = 0; i < Keybinds::GlobalActions.size(); ++i) {
    if (Keybinds::GlobalActions[i] != kind) {
      continue;
    }
    mGlobalKeybinds[i] = slots;
    if (mPersistent) {
      PanelConfig::WriteGlobalKeybinds(mDocument, kind, slots);
      mStore.Save(Spec().mPath, mDocument);// debounced (ST-1)
    }
    mChanged.Emit();
    return;
  }
}

void PanelManager::SetKeyboard(const KeyboardSettings& keyboard) {
  const auto clean = KeyboardConfig::Clean(keyboard);
  if (clean == mKeyboard) {
    return;
  }
  mKeyboard = clean;
  if (mPersistent) {
    KeyboardConfig::Write(mDocument, mKeyboard);
    mStore.Save(Spec().mPath, mDocument);// debounced (ST-1)
  }
  Push();// its panel may have moved, come or gone (only what changed is redone: ST-6)
}

void PanelManager::SetQuickMenu(const QuickMenuSettings& menu) {
  const auto clean = QuickMenuConfig::Clean(menu);
  if (clean == mQuickMenu) {
    return;
  }
  mQuickMenu = clean;
  if (mPersistent) {
    QuickMenuConfig::Write(mDocument, mQuickMenu);
    mStore.Save(Spec().mPath, mDocument);// debounced (ST-1)
  }
  Push();// its panel may have moved
}

void PanelManager::SetHands(const HandSettings& hands) {
  if (hands == mHands) {
    return;
  }
  const bool enabledChanged = hands.mEnabled != mHands.mEnabled;
  mHands = hands;
  if (mPersistent) {
    HandConfig::Write(mDocument, mHands);
    mStore.Save(Spec().mPath, mDocument);// debounced (ST-1)
  }
  if (enabledChanged) {
    Push();// the keyboard and the quick menu come and go with the hands
  } else {
    mChanged.Emit();
  }
}

void PanelManager::Push() {
  std::vector<PanelDefinition> enabled;
  for (const auto& panel: mPanels) {
    if (panel.mEnabled) {
      enabled.push_back(panel);
    }
  }
  mHidden = Visibility::Prune(mPanels, mHidden);
  // The keyboard first (VK-5), while it and the hands are on (K10); the
  // quick menu while the hands are on (it's tapped, ADR-041)
  const bool keyboard = mKeyboard.mEnabled && mHands.mEnabled;
  mRenderer.SetPanels(
    PanelsForRenderer(std::move(enabled), keyboard ? &mKeyboard : nullptr, mHands.mEnabled ? &mQuickMenu : nullptr), mHidden);
  mChanged.Emit();
}

}// namespace OpenDesktopVR::Host
