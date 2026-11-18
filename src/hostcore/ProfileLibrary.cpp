// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PanelConfig.hpp>
#include <OpenDesktopVR/ProfileLibrary.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <system_error>

namespace OpenDesktopVR::Host {

using JsonUtil::Json;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t MaxGames = 64;
constexpr std::size_t MaxNameLength = 100;

// Panel IDs unique within the profile: a repeat gets a new one
void UniquePanelIDs(std::vector<PanelDefinition>& panels) {
  std::set<uint64_t> seen;
  for (auto& panel: panels) {
    while (panel.mID == 0 || panel.mID == KeyboardPanelID || !seen.insert(panel.mID).second) {
      panel.mID = NewPanelID();
    }
  }
}

}// namespace

ProfileLibrary::ProfileLibrary(Settings::Store& store, fs::path folder) : mStore(store), mFolder(std::move(folder)) {
}

std::string ProfileLibrary::NewID() {
  return std::format("{:016x}", NewPanelID());
}

bool ProfileLibrary::IsValidID(std::string_view id) noexcept {
  return id.size() == 16 && std::ranges::all_of(id, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

fs::path ProfileLibrary::PathOf(std::string_view id) const {
  return mFolder / (StringUtil::Widen(id) + L".json");
}

Settings::DocumentSpec ProfileLibrary::Spec(std::string_view id) const {
  return {
    .mPath = PathOf(id),
    .mCurrentVersion = SchemaVersion,
    .mDefaults = [] { return Json::object(); },
  };
}

Json ProfileLibrary::ToJson(const Profile& profile) {
  Json json = profile.mDocument.is_object() ? profile.mDocument : Json::object();
  json["id"] = profile.mID;
  json["name"] = StringUtil::Narrow(profile.mName);
  Json games = Json::array();
  for (const auto& game: profile.mGames) {
    games.push_back(StringUtil::Narrow(game));
  }
  json["games"] = std::move(games);
  PanelConfig::Write(json, profile.mPanels);
  json[std::string {Settings::VersionKey}] = SchemaVersion;
  return json;
}

std::optional<Profile> ProfileLibrary::FromJson(const Json& json, std::size_t* skippedPanels) noexcept {
  try {
    if (!json.is_object()) {
      return std::nullopt;
    }
    Profile profile;
    profile.mID = JsonUtil::GetOr<std::string>(json, "id", "");
    auto name = StringUtil::Widen(JsonUtil::GetOr<std::string>(json, "name", ""));
    if (name.size() > MaxNameLength) {
      name.resize(MaxNameLength);
    }
    profile.mName = name.empty() ? L"Profile" : std::move(name);
    if (const auto games = JsonUtil::Get<Json>(json, "games"); games && games->is_array()) {
      for (const auto& game: *games) {
        if (game.is_string() && profile.mGames.size() < MaxGames) {
          const auto file = fs::path {StringUtil::Widen(game.get<std::string>())}.filename().wstring();
          if (!file.empty()) {
            profile.mGames.push_back(file);
          }
        }
      }
    }
    profile.mPanels = PanelConfig::Read(json, skippedPanels).value_or(std::vector<PanelDefinition> {});
    UniquePanelIDs(profile.mPanels);
    profile.mDocument = json;
    return profile;
  } catch (...) {
    return std::nullopt;
  }
}

std::size_t ProfileLibrary::Load() {
  mProfiles.clear();
  std::size_t unreadable = 0;
  std::error_code error;
  std::vector<fs::path> files;
  for (const auto& entry: fs::directory_iterator(mFolder, error)) {
    const auto& path = entry.path();
    if (entry.is_regular_file(error) && path.extension() == L".json" && IsValidID(StringUtil::Narrow(path.stem().wstring()))) {
      files.push_back(path);
    }
  }
  std::ranges::sort(files);
  for (const auto& path: files) {
    const auto id = StringUtil::Narrow(path.stem().wstring());
    auto loaded = mStore.Load(Spec(id));
    if (loaded.mStatus == Settings::LoadStatus::Unreadable || loaded.mStatus == Settings::LoadStatus::MigrationFailed) {
      ++unreadable;
      Log::Warning("Profiles: {} couldn't be read ({}); set aside, skipped", StringUtil::Narrow(path.filename().wstring()),
        loaded.mProblem);
      continue;
    }
    std::size_t skipped = 0;
    auto profile = FromJson(loaded.mDocument, &skipped);
    if (!profile) {
      ++unreadable;
      continue;
    }
    profile->mID = id;// the file's name wins over what it says inside
    if (skipped > 0) {
      Log::Warning("Profiles: '{}': {} panel entr{} couldn't be read and {} skipped (kept in the file)",
        StringUtil::Narrow(profile->mName), skipped, skipped == 1 ? "y" : "ies", skipped == 1 ? "was" : "were");
    }
    mProfiles.push_back(std::move(*profile));
  }
  return unreadable;
}

const Profile* ProfileLibrary::Find(std::string_view id) const noexcept {
  for (const auto& profile: mProfiles) {
    if (profile.mID == id) {
      return &profile;
    }
  }
  return nullptr;
}

std::string ProfileLibrary::Create(std::wstring name, std::vector<PanelDefinition> panels, std::vector<std::wstring> games) {
  Profile profile;
  do {
    profile.mID = NewID();
  } while (Find(profile.mID));
  if (name.size() > MaxNameLength) {
    name.resize(MaxNameLength);
  }
  profile.mName = name.empty() ? L"Profile" : std::move(name);
  profile.mGames = std::move(games);
  profile.mPanels = std::move(panels);
  UniquePanelIDs(profile.mPanels);
  profile.mDocument = Json::object();
  mStore.Save(PathOf(profile.mID), ToJson(profile));
  mProfiles.push_back(std::move(profile));
  return mProfiles.back().mID;
}

bool ProfileLibrary::Update(const Profile& changed) {
  for (auto& profile: mProfiles) {
    if (profile.mID == changed.mID) {
      profile = changed;
      UniquePanelIDs(profile.mPanels);
      profile.mDocument = ToJson(profile);
      mStore.Save(PathOf(profile.mID), profile.mDocument);// debounced (ST-1)
      return true;
    }
  }
  return false;
}

bool ProfileLibrary::Remove(std::string_view id) {
  if (mProfiles.size() <= 1 || !Find(id)) {
    return false;
  }
  if (!mStore.Delete(PathOf(id))) {// flushes first (ST-3)
    Log::Warning("Profiles: {}.json couldn't be deleted (in use?); kept", id);
    return false;
  }
  std::erase_if(mProfiles, [&](const Profile& profile) { return profile.mID == id; });
  return true;
}

bool ProfileLibrary::Export(std::string_view id, const fs::path& to) {
  const auto* profile = Find(id);
  if (!profile) {
    return false;
  }
  mStore.Flush();// ST-3
  const auto result = FileUtil::WriteAtomic(to, JsonUtil::Dump(ToJson(*profile)));
  return result.mOutcome != FileUtil::WriteOutcome::Failed;
}

std::optional<std::string> ProfileLibrary::Import(const fs::path& from) {
  const auto text = FileUtil::ReadAll(from);
  if (!text || text->size() > 4 * 1024 * 1024) {
    return std::nullopt;
  }
  const auto json = JsonUtil::Parse(*text);
  if (!json) {
    return std::nullopt;
  }
  std::size_t skipped = 0;
  auto profile = FromJson(*json, &skipped);
  if (!profile || !json->contains("panels")) {
    return std::nullopt;
  }
  if (skipped > 0) {
    Log::Warning("Profiles: importing '{}': {} panel entries couldn't be read and were skipped",
      StringUtil::Narrow(profile->mName), skipped);
  }
  return Create(std::move(profile->mName), std::move(profile->mPanels), std::move(profile->mGames));
}

std::optional<std::string> ProfileLibrary::ForGame(std::wstring_view executable) const {
  const auto file = fs::path {executable}.filename().wstring();
  if (file.empty()) {
    return std::nullopt;
  }
  for (const auto& profile: mProfiles) {
    for (const auto& game: profile.mGames) {
      if (StringUtil::EqualsIgnoreCase(game, file)) {
        return profile.mID;
      }
    }
  }
  return std::nullopt;
}

}// namespace OpenDesktopVR::Host
