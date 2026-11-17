// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Utility files: strings, files (atomic writes, ST-2), time, lock checks
// (TH-2), the catch-all (TH-8), product names and the frame timer.

#include "check.hpp"

#include <OpenDesktopVR/Autostart.hpp>

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/FrameWakeup.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <Windows.h>

#include <winrt/base.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <set>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::atomic<int> gViolations {0};
void CountViolation(const char*, const void*) {
  ++gViolations;
}

std::size_t TempFilesIn(const fs::path& folder) {
  std::size_t count = 0;
  std::error_code ec;
  for (const auto& entry: fs::directory_iterator(folder, ec)) {
    if (entry.path().extension() == L".tmp") {
      ++count;
    }
  }
  return count;
}

}// namespace

int main() {
  Scenario("UTF-8 and UTF-16 conversions round-trip", [] {
    const std::string utf8 = "H\xC3\xA4nde \xE2\x9C\x8B";// "Hände ✋"
    const auto wide = StringUtil::Widen(utf8);
    Check(wide == L"H\u00E4nde \u270B", "widened correctly");
    Check(StringUtil::Narrow(wide) == utf8, "narrowed back to the same bytes");
    Check(StringUtil::Widen("").empty() && StringUtil::Narrow(L"").empty(), "empty strings stay empty");
  });

  Scenario("Truncation never splits a character", [] {
    const std::string text = "ab\xC3\xA4";// 'a' 'b' 'ä'(2 bytes)
    Check(StringUtil::TruncateUtf8(text, 3) == "ab", "cut before a two-byte character");
    Check(StringUtil::TruncateUtf8(text, 4) == text, "fits exactly");
    Check(StringUtil::TrimIncompleteUtf8Tail("ab\xE2\x9C") == "ab", "a cut three-byte character is removed");
    Check(StringUtil::TrimIncompleteUtf8Tail("ab\xE2\x9C\x8B") == "ab\xE2\x9C\x8B", "a whole one is kept");
    char buffer[4];
    Check(StringUtil::CopyTruncated("hands", buffer) == 3 && std::string {buffer} == "han", "copied with a terminator");
  });

  Scenario("Small string helpers", [] {
    Check(StringUtil::EqualsIgnoreCase(L"EliteDangerous64.exe", L"elitedangerous64.EXE"), "case-insensitive match");
    Check(!StringUtil::EqualsIgnoreCase(L"a.exe", L"b.exe"), "different names differ");
    Check(StringUtil::ContainsIgnoreCase(L"Untitled - Notepad", L"NOTEPAD"), "contains, ignoring case");
    Check(StringUtil::ContainsIgnoreCase(L"Discord", L""), "an empty search is found");
    Check(!StringUtil::ContainsIgnoreCase(L"note", L"notepad"), "longer than the text: not found");
    Check(!StringUtil::ContainsIgnoreCase(L"Spotify", L"chrome"), "absent: not found");
    Check(StringUtil::FileNamePart(L"C:\\Games\\Elite\\Game.exe") == L"Game.exe", "file name of a path");
  });

  Scenario("Product names (ADR-017, ADR-019)", [] {
    Check(Product::LayerName == "XR_APILAYER_OPENDESKTOPVR_overlay", "the layer name is the decided one");
    Check(
      Product::KernelObjectName(L"Panels", 3) == L"Local\\OpenDesktopVR.v3.Panels",
      "kernel object names are session-local, versioned and prefixed");
    const auto data = Product::DataFolder();
    Check(!data.empty() && data.filename() == L"OpenDesktopVR", "the data folder is %LOCALAPPDATA%\\OpenDesktopVR");
  });

  Scenario("Atomic writes replace the file and leave no temp files (ST-1)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"sub" / L"settings.json";
    auto result = FileUtil::WriteAtomic(file, "first");
    Check(result.mOutcome == FileUtil::WriteOutcome::Atomic, "first write is atomic, and creates the folder");
    result = FileUtil::WriteAtomic(file, "second");
    Check(FileUtil::ReadAll(file) == "second", "the second write replaced the first");
    Check(TempFilesIn(file.parent_path()) == 0, "no temp files left behind");
  });

  Scenario("A held file: the rename is retried, then written in place (ST-2)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"held.json";
    FileUtil::WriteAtomic(file, "original");
    // Like an antivirus scanner: reading, sharing read and write but not delete
    HANDLE holder = CreateFileW(
      file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(holder != INVALID_HANDLE_VALUE, "the file is held open");

    const auto start = TimeUtil::Now();
    auto result = FileUtil::WriteAtomic(file, "updated");
    const auto ms = TimeUtil::ToMilliseconds(TimeUtil::Now() - start);
    Check(result.mRenameRetries == 2, std::format("the rename was retried twice (got {})", result.mRenameRetries));
    Check(ms >= 15 && ms < 500, std::format("about 3 x 10 ms spent retrying ({:.0f} ms)", ms));
    Check(result.mOutcome == FileUtil::WriteOutcome::InPlaceFallback, "fell back to writing in place");
    Check(FileUtil::ReadAll(file) == "updated", "the new contents were saved");

    result = FileUtil::WriteAtomic(file, "not saved", {.mAllowInPlaceFallback = false});
    Check(result.mOutcome == FileUtil::WriteOutcome::Failed, "without the fallback, the write fails...");
    Check(FileUtil::ReadAll(file) == "updated", "...and the file keeps its previous contents");
    CloseHandle(holder);
    Check(TempFilesIn(folder.Path()) == 0, "no temp files left behind");
  });

  Scenario("Concurrent writers never corrupt each other's temp files (ST-2)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"shared.json";
    std::set<std::string> payloads;
    for (int t = 0; t < 8; ++t) {
      payloads.insert(std::string(1000 + t, static_cast<char>('a' + t)));
    }
    std::vector<std::thread> threads;
    std::atomic<int> failed {0};
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&, t] {
        const std::string payload(1000 + t, static_cast<char>('a' + t));
        for (int i = 0; i < 25; ++i) {
          if (FileUtil::WriteAtomic(file, payload).mOutcome == FileUtil::WriteOutcome::Failed) {
            ++failed;
          }
        }
      });
    }
    for (auto& thread: threads) {
      thread.join();
    }
    const auto contents = FileUtil::ReadAll(file).value_or("");
    Check(payloads.contains(contents), "the final file is one writer's complete contents");
    Check(TempFilesIn(folder.Path()) == 0, "no temp files left behind");
    std::printf("  (%d of 200 writes lost the race and reported failure)\n", failed.load());
  });

  Scenario("CopyAside keeps the original and never overwrites an earlier copy (ST-4)", [] {
    TempFolder folder;
    const fs::path file = fs::path {folder.Path()} / L"profile.json";
    FileUtil::WriteAtomic(file, "{broken");
    const auto first = FileUtil::CopyAside(file, L"unreadable");
    const auto second = FileUtil::CopyAside(file, L"unreadable");
    Check(first && first->filename() == L"profile.json.unreadable", "first copy is <name>.unreadable");
    Check(second && second->filename() == L"profile.json.unreadable.1", "second copy gets a number");
    Check(FileUtil::ReadAll(*first) == "{broken" && FileUtil::Exists(file), "contents copied, original kept");
  });

  Scenario("Reading a missing file is a result, not an exception", [] {
    Check(!FileUtil::ReadAll(L"Z:\\does\\not\\exist.json"), "nullopt for a missing file");
    Check(!FileUtil::Size(L"Z:\\does\\not\\exist.json"), "no size for a missing file");
  });

  Scenario("Rate limiter and periodic timer", [] {
    TimeUtil::RateLimiter limiter {1s};
    const auto t0 = TimeUtil::Now();
    Check(limiter.Allow(t0), "the first call is allowed");
    Check(!limiter.Allow(t0 + 500ms), "half a second later it isn't");
    Check(limiter.Allow(t0 + 1s), "a second later it is");
    TimeUtil::PeriodicTimer timer {60s};
    Check(!timer.Due(t0), "the timer starts counting on first use");
    Check(!timer.Due(t0 + 59s) && timer.Due(t0 + 60s), "due after 60 s");
    Check(timer.Due(t0 + 300s) && !timer.Due(t0 + 301s), "missed periods fire once, not five times");
  });

  Scenario("The catch-all swallows std and Windows Runtime errors (TH-8)", [] {
    Check(!ThreadUtil::CatchAll("test", [] { throw std::runtime_error("boom"); }), "std::exception caught");
    Check(!ThreadUtil::CatchAll("test", [] { winrt::check_hresult(E_ACCESSDENIED); }), "winrt::hresult_error caught");
    Check(!ThreadUtil::CatchAll("test", [] { throw 42; }), "anything else caught");
    Check(ThreadUtil::CatchAll("test", [] {}), "no exception: true");
    std::string described;
    try {
      winrt::check_hresult(E_ACCESSDENIED);
    } catch (...) {
      described = ThreadUtil::DescribeCurrentException();
    }
    Check(described.starts_with("Windows Runtime error 0x80070005"), "a WinRT error is described by its HRESULT");
  });

  Scenario("Lock checks catch re-entrant locking (TH-2)", [] {
    ThreadUtil::LockCheck::SetEnabled(true);
    ThreadUtil::LockCheck::SetViolationHandler(&CountViolation);
    ThreadUtil::CheckedMutex mutex;
    ThreadUtil::CheckedSharedMutex shared;
    {
      std::scoped_lock lock(mutex);
      Check(ThreadUtil::LockCheck::HeldByThisThread() == 1, "lock depth 1 while held");
      Check(!mutex.try_lock(), "re-locking the same mutex fails...");
      Check(gViolations == 1, "...and is reported");
      std::shared_lock sharedLock(shared);
      Check(ThreadUtil::LockCheck::HeldByThisThread() == 2, "lock depth 2 with a second lock");
      shared.lock_shared();// a second shared lock on the same thread
      Check(gViolations == 2, "taking a shared lock twice is reported");
      shared.unlock_shared();
    }
    Check(ThreadUtil::LockCheck::HeldByThisThread() == 0, "lock depth back to 0");
    std::thread other([&] {
      std::scoped_lock lock(mutex);// another thread: fine
    });
    other.join();
    Check(gViolations == 2, "different threads taking the same lock is not a violation");
    ThreadUtil::LockCheck::SetViolationHandler(nullptr);
  });

  Scenario("The frame timer wakes when asked, not on the next 15.6 ms tick (test round 5)", [] {
    bool highResolution = false;
    const HANDLE timer = FrameWakeup::CreateTimer(&highResolution);
    Check(timer != nullptr && highResolution, "a high-resolution timer");
    if (!timer) {
      return;
    }
    // 20 waits of 2 ms; a tick-bound timer would average 8-16 ms
    double total = 0;
    for (int i = 0; i < 20; ++i) {
      const auto start = TimeUtil::Now();
      const LARGE_INTEGER due {.QuadPart = -20'000};// 2 ms, relative, in 100 ns
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 1000);
      total += TimeUtil::ToMilliseconds(TimeUtil::Now() - start);
    }
    CloseHandle(timer);
    Check(total / 20 < 5.0, std::format("2 ms waits took {:.2f} ms on average", total / 20));
  });

  Scenario("Start with Windows: on, off, and only for this program (UI-7), in a test key", [] {
    // A private key, never the real Run key; removed afterwards
    const auto parent = std::format(L"Software\\OpenDesktopVR-Test-{}", GetCurrentProcessId());
    const auto key = parent + L"\\Run";
    const std::filesystem::path exe {L"C:\\Program Files\\Open Desktop VR\\OpenDesktopVR.exe"};
    Check(!Autostart::IsEnabled(exe, key.c_str()), "off at first");
    Check(Autostart::SetEnabled(true, exe, key.c_str()) && Autostart::IsEnabled(exe, key.c_str()), "turned on");
    Check(!Autostart::IsEnabled(L"C:\\Elsewhere\\OpenDesktopVR.exe", key.c_str()), "another copy of the program isn't it");
    Check(Autostart::SetEnabled(false, exe, key.c_str()) && !Autostart::IsEnabled(exe, key.c_str()), "turned off");
    Check(Autostart::SetEnabled(false, exe, key.c_str()), "turning it off again is fine");
    RegDeleteTreeW(HKEY_CURRENT_USER, parent.c_str());
  });

  return Finish();
}
