// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Profiles (doc 09 "Profiles"; ADR-038): one file per profile in a folder,
// `<id>.json`, each a full set of panels plus the games that claim it:
//
//   {"schemaVersion": 1, "id": "1a2b3c4d5e6f7081", "name": "Elite",
//    "games": ["EliteDangerous64.exe"], "panels": [ ... ]}
//
// - Every file goes through the settings store: debounced atomic writes
//   (ST-1, ST-2); an unreadable one is set aside and skipped (ST-4);
//   unknown fields survive (ST-5).
// - Removing and exporting flush pending writes first (ST-3).
// - An imported file is untrusted (TH-9): read like any profile, given a
//   new ID so it never replaces one; bad entries are skipped.
// - Panel IDs are unique within a profile (repeats get new ones).
// Not thread-safe: the UI thread owns it.
#pragma once

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Host {

struct Profile {
  std::string mID;// 16 hex digits
  std::wstring mName;
  // Executable file names that claim it ("EliteDangerous64.exe")
  std::vector<std::wstring> mGames;
  std::vector<PanelDefinition> mPanels;
  // The file as read, so unknown fields survive (ST-5)
  JsonUtil::Json mDocument;
};

class ProfileLibrary {
 public:
  static constexpr int SchemaVersion = 1;

  ProfileLibrary(Settings::Store& store, std::filesystem::path folder);

  // Reads every profile file; returns how many were unreadable (set aside)
  std::size_t Load();
  const std::vector<Profile>& All() const noexcept {
    return mProfiles;
  }
  const Profile* Find(std::string_view id) const noexcept;

  // A new profile, saved; its ID
  std::string Create(std::wstring name, std::vector<PanelDefinition> panels, std::vector<std::wstring> games = {});
  // Saves a changed profile (debounced); false if its ID is unknown
  bool Update(const Profile& profile);
  // Flushes, then deletes its file; never the last profile
  bool Remove(std::string_view id);
  // One profile as one file, written atomically after a flush
  bool Export(std::string_view id, const std::filesystem::path& to);
  // A profile file from anywhere: a new profile with a new ID, or nullopt
  // if it isn't one
  std::optional<std::string> Import(const std::filesystem::path& from);

  // The profile claiming this executable (its file name, any case)
  std::optional<std::string> ForGame(std::wstring_view executable) const;

  std::filesystem::path PathOf(std::string_view id) const;
  static std::string NewID();
  static bool IsValidID(std::string_view id) noexcept;

  // The file's form, and back (untrusted: anything malformed is dropped)
  static JsonUtil::Json ToJson(const Profile& profile);
  static std::optional<Profile> FromJson(const JsonUtil::Json& json, std::size_t* skippedPanels = nullptr) noexcept;

 private:
  Settings::DocumentSpec Spec(std::string_view id) const;

  Settings::Store& mStore;
  std::filesystem::path mFolder;
  std::vector<Profile> mProfiles;
};

}// namespace OpenDesktopVR::Host
