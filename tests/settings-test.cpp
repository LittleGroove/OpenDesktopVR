// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Settings store (ST-1 to ST-5; doc 11 "Settings write storm" and the
// corrupt-file robustness cases).

#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/SettingsStore.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <Windows.h>

#include <filesystem>
#include <format>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Settings;
using namespace OpenDesktopVR::Testing;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

DocumentSpec Spec(const fs::path& path, int version = 1) {
  return {
    .mPath = path,
    .mCurrentVersion = version,
    .mDefaults = [] { return Json {{"handsVisible", true}, {"maxUpdatesPerSecond", 30}}; },
  };
}

Json FileJson(const fs::path& path) {
  return JsonUtil::Parse(FileUtil::ReadAll(path).value_or("")).value_or(Json {});
}

}// namespace

int main() {
  Scenario("Write storm: 300 rapid changes, at most 2 disk writes, under 1 s (ST-1)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    Store store;
    const auto start = TimeUtil::Now();
    for (int i = 0; i < 300; ++i) {
      store.Save(file, Json {{"schemaVersion", 1}, {"opacity", i}});
    }
    const auto callerMs = TimeUtil::ToMilliseconds(TimeUtil::Now() - start);
    std::this_thread::sleep_for(450ms);// past the debounce
    const auto counters = store.Counters();
    Check(callerMs < 1000, std::format("300 changes took {:.1f} ms on the calling thread", callerMs));
    Check(counters.mDiskWrites <= 2, std::format("{} disk write(s) for 300 changes", counters.mDiskWrites));
    Check(counters.mDiskWrites >= 1, "the debounced write happened without a flush");
    Check(JsonUtil::GetOr<int>(FileJson(file), "opacity", -1) == 299, "the file has the last value");
  });

  Scenario("Changes spread over 0.9 s still coalesce (debounce + 2 s cap)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    Store store;
    // Bounded by time, not count: Sleep granularity can be ~15 ms
    const auto start = TimeUtil::Now();
    int changes = 0;
    while (TimeUtil::Now() - start < 900ms) {
      store.Save(file, Json {{"schemaVersion", 1}, {"opacity", changes++}});
      std::this_thread::sleep_for(3ms);
    }
    store.Flush();
    Check(
      store.Counters().mDiskWrites <= 2,
      std::format("{} disk write(s) for {} changes over 0.9 s", store.Counters().mDiskWrites, changes));
  });

  Scenario("Unknown fields survive a load-edit-save round trip (ST-5)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    FileUtil::WriteAtomic(
      file, R"({"schemaVersion": 1, "maxUpdatesPerSecond": 20, "fromTheFuture": {"gesture": "wave"}})");
    Store store;
    auto loaded = store.Load(Spec(file));
    Check(loaded.mStatus == LoadStatus::Loaded, "loaded");
    Check(JsonUtil::GetOr<bool>(loaded.mDocument, "handsVisible", false), "a missing key is filled from defaults");
    loaded.mDocument["maxUpdatesPerSecond"] = 25;
    store.Save(file, loaded.mDocument);
    store.Flush();
    const auto saved = FileJson(file);
    Check(JsonUtil::GetOr<int>(saved, "maxUpdatesPerSecond", 0) == 25, "the edit was saved");
    Check(
      saved.contains("fromTheFuture") && saved["fromTheFuture"].value("gesture", "") == "wave",
      "the unknown field is still there");
  });

  Scenario("Corrupt files never block startup; they're kept aside (ST-4)", [] {
    TempFolder folder;
    const std::pair<const char*, std::string> cases[] = {
      {"empty", ""},
      {"truncated", R"({"schemaVersion": 1, "handsVis)"},
      {"garbage", std::string("\x00\xFF\xFE garbage \x01", 14)},
      {"not an object", R"([1, 2, 3])"},
    };
    Store store;
    for (const auto& [name, contents]: cases) {
      const fs::path file = fs::path {folder.Path()} / std::format(L"{}.json", StringUtil::Widen(name));
      FileUtil::WriteAtomic(file, contents);
      const auto loaded = store.Load(Spec(file));
      Check(loaded.mStatus == LoadStatus::Unreadable, std::format("{}: reported unreadable", name));
      Check(
        JsonUtil::GetOr<int>(loaded.mDocument, "maxUpdatesPerSecond", 0) == 30,
        std::format("{}: started from defaults", name));
      Check(
        loaded.mCopiedTo && FileUtil::ReadAll(*loaded.mCopiedTo) == contents,
        std::format("{}: the original bytes are kept as .unreadable", name));
    }
  });

  Scenario("One bad panel entry is skipped; the rest load (ST-4)", [] {
    const auto profile = JsonUtil::Parse(R"({"panels": [
      {"id": "a", "width": 0.3},
      {"id": 42, "width": 0.3},
      "not even an object",
      {"id": "c", "width": 0.5}
    ]})");
    struct Panel {
      std::string mID;
      double mWidth;
    };
    std::size_t skipped = 0;
    const auto panels = JsonUtil::ParseEach<Panel>(
      profile->at("panels"),
      [](const Json& entry) -> std::optional<Panel> {
        auto id = JsonUtil::Get<std::string>(entry, "id");
        auto width = JsonUtil::Get<double>(entry, "width");
        if (!id || !width) {
          return std::nullopt;
        }
        return Panel {*id, *width};
      },
      &skipped);
    Check(panels.size() == 2 && panels[0].mID == "a" && panels[1].mID == "c", "the two good panels loaded");
    Check(skipped == 2, "two bad entries skipped");
  });

  Scenario("Old versions are backed up, then migrated (ST-5)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    const std::string original = R"({"schemaVersion": 1, "fps": 45, "keepMe": true})";
    FileUtil::WriteAtomic(file, original);
    auto spec = Spec(file, 3);
    spec.mMigrations = {
      {.mFromVersion = 1,
       .mApply =
         [](Json& doc) {
           doc["maxUpdatesPerSecond"] = doc["fps"];
           doc.erase("fps");
           return true;
         }},
      {.mFromVersion = 2,
       .mApply =
         [](Json& doc) {
           doc["handsVisible"] = false;
           return true;
         }},
    };
    Store store;
    const auto loaded = store.Load(spec);
    Check(loaded.mStatus == LoadStatus::Migrated && loaded.mFileVersion == 1, "migrated from version 1");
    Check(JsonUtil::GetOr<int>(loaded.mDocument, "maxUpdatesPerSecond", 0) == 45, "1 -> 2 renamed the key");
    Check(!JsonUtil::GetOr<bool>(loaded.mDocument, "handsVisible", true), "2 -> 3 set its value (not the default)");
    Check(loaded.mCopiedTo && FileUtil::ReadAll(*loaded.mCopiedTo) == original, "the backup has the old file");
    store.Flush();
    const auto saved = FileJson(file);
    Check(JsonUtil::GetOr<int>(saved, std::string {VersionKey}, 0) == 3, "the file was rewritten at version 3");
    Check(JsonUtil::GetOr<bool>(saved, "keepMe", false), "unrelated fields survived the migration");
  });

  Scenario("A failed migration keeps the backup and uses defaults", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    FileUtil::WriteAtomic(file, R"({"schemaVersion": 1})");
    Store store;
    const auto loaded = store.Load(Spec(file, 2));// no migration from 1
    Check(loaded.mStatus == LoadStatus::MigrationFailed, "reported as a failed migration");
    Check(loaded.mCopiedTo && FileUtil::Exists(*loaded.mCopiedTo), "the backup exists");
    Check(JsonUtil::GetOr<int>(loaded.mDocument, "maxUpdatesPerSecond", 0) == 30, "defaults used");
  });

  Scenario("A newer build's file is used as-is", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    FileUtil::WriteAtomic(file, R"({"schemaVersion": 9, "maxUpdatesPerSecond": 60, "newThing": 1})");
    Store store;
    const auto loaded = store.Load(Spec(file, 1));
    Check(loaded.mStatus == LoadStatus::NewerVersion, "reported as newer");
    Check(
      JsonUtil::GetOr<int>(loaded.mDocument, "newThing", 0) == 1
        && JsonUtil::GetOr<int>(loaded.mDocument, std::string {VersionKey}, 0) == 9,
      "its fields and version are kept");
  });

  Scenario("Pending writes are flushed before reads and deletes (ST-3)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"profile.json";
    Store store;
    store.Save(file, Json {{"schemaVersion", 1}, {"maxUpdatesPerSecond", 12}});
    const auto loaded = store.Load(Spec(file));
    Check(JsonUtil::GetOr<int>(loaded.mDocument, "maxUpdatesPerSecond", 0) == 12, "Load sees the queued change");

    store.Save(file, Json {{"schemaVersion", 1}});
    Check(store.Delete(file), "Delete succeeded");
    std::this_thread::sleep_for(400ms);
    Check(!FileUtil::Exists(file), "the queued write didn't recreate the deleted file later");
  });

  Scenario("A held file is still saved, with a warning (ST-2)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    FileUtil::WriteAtomic(file, "{}");
    HANDLE holder = CreateFileW(
      file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    Store store;
    int outcomes = 0;
    FileUtil::WriteOutcome last {};
    auto sub = store.OnWritten().Subscribe([&](const fs::path&, FileUtil::WriteOutcome outcome) {
      ++outcomes;
      last = outcome;
    });
    store.Save(file, Json {{"schemaVersion", 1}, {"held", true}});
    store.Flush();
    CloseHandle(holder);
    const auto counters = store.Counters();
    Check(counters.mRenameRetries == 2 && counters.mInPlaceFallbacks == 1, "retried twice, then wrote in place");
    Check(outcomes == 1 && last == FileUtil::WriteOutcome::InPlaceFallback, "OnWritten reported the fallback");
    Check(JsonUtil::GetOr<bool>(FileJson(file), "held", false), "the contents were saved");
  });

  Scenario("Destroying the store flushes what's pending", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"settings.json";
    {
      Store store;
      store.Save(file, Json {{"schemaVersion", 1}, {"shutdown", true}});
    }
    Check(JsonUtil::GetOr<bool>(FileJson(file), "shutdown", false), "written on shutdown");
  });

  return Finish();
}
