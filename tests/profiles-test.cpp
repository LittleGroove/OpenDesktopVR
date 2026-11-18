// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Profiles (M6a, ADR-038), in a temporary folder:
// - made, saved, read back; unknown fields survive (ST-5);
// - an unreadable profile file is set aside and skipped; the rest load
//   (ST-4); one bad panel entry doesn't fail its profile;
// - the last profile can't be removed; removing flushes first (ST-3);
// - export, then import: a new profile with a new ID, the same panels and
//   games; a file that isn't a profile imports nothing (TH-9);
// - the game that claims a profile, by executable file name in any case;
// - repeated panel IDs within a profile are made unique;
// - settings.json version 1 is backed up and migrated to version 2;
// - the game followed: an ended game whose ID lingers in the stats is
//   never followed again (test round 15: profiles flipped every second).

#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/GameFollower.hpp>
#include <OpenDesktopVR/GlobalSettings.hpp>
#include <OpenDesktopVR/ProfileLibrary.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <set>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Host;
using Testing::Check;
using Testing::Scenario;
namespace fs = std::filesystem;

namespace {

PanelDefinition Panel(uint64_t id, const wchar_t* name) {
  PanelDefinition panel;
  panel.mID = id;
  panel.mName = name;
  return panel;
}

void WriteText(const fs::path& path, std::string_view text) {
  std::ofstream {path, std::ios::binary} << text;
}

}// namespace

int main() {
  Testing::TempFolder temp;
  const fs::path folder = fs::path {temp.Path()} / L"profiles";
  fs::create_directories(folder);

  std::string eliteID;
  Scenario("Profiles are made, saved and read back (ADR-038)", [&] {
    Settings::Store store;
    ProfileLibrary library {store, folder};
    library.Load();
    Check(library.All().empty(), "an empty folder: no profiles");
    eliteID = library.Create(L"Elite", {Panel(1, L"Discord"), Panel(2, L"Spotify")}, {L"EliteDangerous64.exe"});
    library.Create(L"Desk", {Panel(3, L"Notepad")});
    Check(ProfileLibrary::IsValidID(eliteID), std::format("16 hex digits ({})", eliteID));
    store.Flush();
    Check(fs::exists(library.PathOf(eliteID)), "one file each");

    ProfileLibrary again {store, folder};
    again.Load();
    const auto* elite = again.Find(eliteID);
    Check(again.All().size() == 2 && elite && elite->mName == L"Elite" && elite->mPanels.size() == 2
        && elite->mGames == std::vector<std::wstring> {L"EliteDangerous64.exe"},
      "read back: names, panels and games");
  });

  Scenario("Unknown fields survive; an unreadable file is set aside; a bad panel entry is skipped (ST-4, ST-5)", [&] {
    Settings::Store store;
    {
      ProfileLibrary library {store, folder};
      library.Load();
      auto profile = *library.Find(eliteID);
      profile.mDocument["fromANewerVersion"] = "kept";
      library.Update(profile);
      profile = *library.Find(eliteID);
      profile.mName = L"Elite Dangerous";
      library.Update(profile);
      store.Flush();
    }
    // A garbage profile, and one with a bad panel entry
    WriteText(folder / L"00000000000000aa.json", "{ this isn't json");
    WriteText(folder / L"00000000000000bb.json",
      R"({"schemaVersion": 1, "name": "Mixed", "panels": [{"id": "zz", "kind": "nonsense"}, {"id": "00000000000000c1", "name": "Good", "kind": "testPattern"}]})");
    ProfileLibrary library {store, folder};
    const auto unreadable = library.Load();
    Check(unreadable == 1, std::format("the garbage file counted as unreadable ({})", unreadable));
    Check(!library.Find("00000000000000aa"), "and skipped");
    bool asideFound = false;
    for (const auto& entry: fs::directory_iterator(folder)) {
      asideFound = asideFound || entry.path().wstring().find(L".unreadable") != std::wstring::npos;
    }
    Check(asideFound, "a copy of it kept aside");
    const auto* mixed = library.Find("00000000000000bb");
    Check(mixed && mixed->mPanels.size() == 1 && mixed->mPanels[0].mName == L"Good", "one bad panel entry doesn't fail its profile");
    const auto* elite = library.Find(eliteID);
    Check(elite && elite->mName == L"Elite Dangerous" && JsonUtil::GetOr<std::string>(elite->mDocument, "fromANewerVersion", "") == "kept",
      "an unknown field survived a rename");
  });

  Scenario("Games claim profiles by executable name, any case", [&] {
    Settings::Store store;
    ProfileLibrary library {store, folder};
    library.Load();
    Check(library.ForGame(L"C:\\Games\\Elite\\ELITEDANGEROUS64.EXE") == eliteID, "a full path, other case: Elite's profile");
    Check(!library.ForGame(L"hello_xr.exe"), "an unclaimed game: none");
    Check(!library.ForGame(L""), "no game: none");
  });

  Scenario("Export, then import: a new profile with the same panels; garbage imports nothing (TH-9)", [&] {
    Settings::Store store;
    ProfileLibrary library {store, folder};
    library.Load();
    const auto file = fs::path {temp.Path()} / L"shared.json";
    Check(library.Export(eliteID, file) && fs::exists(file), "exported to one file");
    const auto imported = library.Import(file);
    const auto* copy = imported ? library.Find(*imported) : nullptr;
    Check(copy && *imported != eliteID, "imported with a new ID, never replacing the original");
    Check(copy && copy->mName == L"Elite Dangerous" && copy->mPanels.size() == 2 && copy->mGames.size() == 1,
      "the same name, panels and games");
    WriteText(fs::path {temp.Path()} / L"garbage.json", "[1, 2, 3]");
    WriteText(fs::path {temp.Path()} / L"other.json", R"({"something": "else"})");
    Check(!library.Import(fs::path {temp.Path()} / L"garbage.json") && !library.Import(fs::path {temp.Path()} / L"other.json")
        && !library.Import(fs::path {temp.Path()} / L"missing.json"),
      "a file that isn't a profile imports nothing");
  });

  Scenario("Repeated panel IDs are made unique; the last profile can't be removed (ST-3)", [&] {
    Settings::Store store;
    const fs::path single = fs::path {temp.Path()} / L"single";
    fs::create_directories(single);
    ProfileLibrary library {store, single};
    const auto id = library.Create(L"Only", {Panel(7, L"A"), Panel(7, L"B"), Panel(0, L"C")});
    const auto& panels = library.Find(id)->mPanels;
    Check(panels.size() == 3 && panels[0].mID != panels[1].mID && panels[2].mID != 0, "each panel has its own ID");
    Check(!library.Remove(id) && library.All().size() == 1, "the last profile stays");
    const auto second = library.Create(L"Second", {});
    Check(library.Remove(second) && !fs::exists(library.PathOf(second)), "another one is removed, file and all");
  });

  Scenario("settings.json version 1: backed up, then version 2 (ADR-038)", [&] {
    const auto file = fs::path {temp.Path()} / L"settings.json";
    WriteText(file, R"({"schemaVersion": 1, "panels": [{"id": "00000000000000d1", "name": "Old", "kind": "testPattern"}], "hands": {"enabled": true}})");
    auto spec = Settings::GlobalSettingsSpec();
    spec.mPath = file;
    Settings::Store store;
    const auto loaded = store.Load(spec);
    Check(loaded.mStatus == Settings::LoadStatus::Migrated && loaded.mCopiedTo && fs::exists(*loaded.mCopiedTo),
      "migrated, with the old file backed up");
    Check(JsonUtil::GetOr<int>(loaded.mDocument, Settings::VersionKey, 0) == 2 && loaded.mDocument.contains("panels"),
      "version 2, the panel list still there for the host to move");
  });

  Scenario("A game that ended is never followed again, though its ID lingers (test round 15)", [] {
    std::set<uint32_t> running {100};
    GameFollower follower {[&](uint32_t id) { return running.contains(id); }};
    const auto started = follower.Update(100, L"EliteDangerous64.exe");
    Check(started && started->mKind == GameFollower::Change::Kind::Started && follower.Executable() == L"EliteDangerous64.exe",
      "Elite's layer attached: followed");
    Check(!follower.Update(100, L"EliteDangerous64.exe"), "the same game a second later: nothing new");
    running.erase(100);
    const auto ended = follower.Update(100, L"EliteDangerous64.exe");
    Check(ended && ended->mKind == GameFollower::Change::Kind::Ended && follower.Executable().empty(), "Elite exited: ended");
    int changes = 0;
    for (int second = 0; second < 10; ++second) {
      changes += follower.Update(100, L"EliteDangerous64.exe") ? 1 : 0;
    }
    Check(changes == 0, std::format("its ID still in the stats for 10 s: never followed again ({} changes)", changes));
    Check(!follower.Update(300, L"gone.exe") && follower.Executable().empty(), "a process already gone when first seen: not followed");
    running.insert(200);
    const auto next = follower.Update(200, L"EliteDangerous64.exe");
    Check(next && next->mKind == GameFollower::Change::Kind::Started, "Elite started again (a new process): followed");
  });

  Scenario("The profile a game replaced: back when it ends, also after a restart (test round 23)", [] {
    using namespace std::chrono_literals;
    const auto t0 = ProfileReturn::Clock::now();
    ProfileReturn back;
    back.Replaced("desk");
    Check(!back.Tick(false, t0 + 60s), "while the game runs, ticks don't switch back");
    Check(back.GameEnded() == "desk" && !back.Remembered(), "the game ended: back to 'desk', then nothing remembered");

    back.Replaced("desk");
    back.Forget();
    Check(!back.GameEnded(), "the user chose a profile meanwhile: no switch back");

    back.Replaced("desk");
    back.ForgetIf("other");
    Check(back.Remembered() == "desk", "another profile deleted: still remembered");
    back.ForgetIf("desk");
    Check(!back.Remembered(), "the remembered profile deleted: forgotten");

    // Restarted mid-game (an update): the game's layer comes back in time
    back.Restore("desk", "elite", t0);
    Check(!back.Tick(false, t0 + 2s), "restored: waits for the game");
    Check(!back.Tick(true, t0 + 3s), "the game's layer is back: keeps waiting for it to end");
    Check(!back.Tick(false, t0 + 60s), "no switch after the grace once the game was seen");
    Check(back.GameEnded() == "desk", "the game ended: back to 'desk'");

    // Started after the game had gone
    back.Restore("desk", "elite", t0);
    Check(!back.Tick(false, t0 + ProfileReturn::Grace - 1s), "no game yet, within the grace: waits");
    Check(back.Tick(false, t0 + ProfileReturn::Grace) == "desk", "no game within the grace: back to 'desk'");
    Check(!back.Tick(false, t0 + 60s) && !back.Remembered(), "once");

    back.Restore("elite", "elite", t0);
    Check(!back.Remembered(), "a remembered profile that's already active is dropped");
    back.Restore(std::nullopt, "elite", t0);
    Check(!back.Tick(false, t0 + 60s), "nothing remembered: nothing happens");
  });

  return Testing::Finish();
}
