// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Logging (LG-1, XR-3, XR-4): bounded queue that drops the oldest entries
// with a notice, truncation without split characters, no allocation on the
// writing thread, file output, and a bounded wait on stop.

#include "check.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/LogRing.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;

// ---- Allocation counting on the current thread (XR-3) -----------------------
namespace {
thread_local bool tCountAllocations = false;
thread_local int tAllocations = 0;
}// namespace

void* operator new(std::size_t size) {
  if (tCountAllocations) {
    ++tAllocations;
  }
  if (void* p = std::malloc(size ? size : 1)) {
    return p;
  }
  throw std::bad_alloc {};
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}

namespace {

struct Captured {
  std::mutex mMutex;
  std::vector<std::string> mLines;
  std::vector<Log::Level> mLevels;

  Events::Subscription Attach() {
    return Log::AddSink([this](const Log::Entry& entry) {
      std::scoped_lock lock(mMutex);
      mLines.emplace_back(entry.mText);
      mLevels.push_back(entry.mLevel);
    });
  }
  std::vector<std::string> Lines() {
    std::scoped_lock lock(mMutex);
    return mLines;
  }
};

bool IsValidUtf8(std::string_view s) {
  return !StringUtil::Widen(s).empty() || s.empty();
}

}// namespace

int main() {
  // Runs first: nothing else may have used the queue yet
  Scenario("A full queue drops the oldest entries, then says how many (LG-1)", [] {
    for (int i = 0; i < 300; ++i) {
      Log::Info("message {}", i);
    }
    Captured captured;
    auto sink = captured.Attach();
    Log::Start({.mDebuggerOutput = false});
    Log::Stop();

    const auto lines = captured.Lines();
    const auto dropped = Log::GetCounters().mDropped;
    Check(dropped >= 44 && dropped <= 45, std::format("44 (or 45) messages dropped (got {})", dropped));
    Check(
      !lines.empty() && lines.front() == std::format("{} log messages dropped (the log queue was full)", dropped),
      "the drop notice comes first, where the gap is");
    Check(
      lines.size() > 1 && lines[1] == std::format("message {}", dropped),
      std::format("the oldest survivor is message {} (got '{}')", dropped, lines.size() > 1 ? lines[1] : ""));
    bool last = false;
    for (const auto& line: lines) {
      last = last || line == "message 299";
    }
    Check(last, "the newest message survived");
    Check(lines.size() <= Log::QueueCapacity + 2, std::format("at most a queue's worth written ({})", lines.size()));
  });

  Scenario("Over-long messages are truncated without splitting a character", [] {
    Captured captured;
    auto sink = captured.Attach();
    Log::Start({.mDebuggerOutput = false});
    const auto before = Log::GetCounters().mTruncated;
    std::string longText;
    while (longText.size() < 2000) {
      longText += "h\xC3\xA4nde ";// "hände", with a two-byte character
    }
    Log::Info("{}", longText);
    Log::Write(Log::Level::Warning, longText);
    Log::Stop();
    const auto lines = captured.Lines();
    bool allFit = !lines.empty();
    bool allValid = true;
    for (const auto& line: lines) {
      allFit = allFit && line.size() <= Log::MaxMessageBytes;
      allValid = allValid && IsValidUtf8(line);
    }
    Check(allFit, std::format("every line fits in {} bytes", Log::MaxMessageBytes));
    Check(allValid, "every line is valid UTF-8 (no half characters)");
    Check(Log::GetCounters().mTruncated - before == 2, "both truncations counted");
  });

  Scenario("Writing a log line allocates nothing on the calling thread (XR-3)", [] {
    Log::Info("warm-up {}", 1);// first use creates the shared state
    const std::string_view name = "panel";
    tAllocations = 0;
    tCountAllocations = true;
    for (int i = 0; i < 50; ++i) {
      Log::Info("frame {}: {}, visible {}", i, name, i % 2 == 0);
      Log::Write(Log::Level::Debug, "plain text");
    }
    tCountAllocations = false;
    Check(tAllocations == 0, std::format("0 allocations in 100 log calls (got {})", tAllocations));

    // Floating-point formatting allocates in the Debug C++ runtime only (its
    // checked conversion path). XR-3 applies to the shipped Release build.
#ifdef NDEBUG
    tAllocations = 0;
    tCountAllocations = true;
    for (int i = 0; i < 50; ++i) {
      Log::Info("panel at {:.3f} m", 0.5 + i);
    }
    tCountAllocations = false;
    Check(tAllocations == 0, std::format("0 allocations formatting 50 floats (got {})", tAllocations));
#endif
  });

  Scenario("Lines reach the log file", [] {
    TempFolder folder;
    const auto file = std::filesystem::path {folder.Path()} / L"logs" / L"test.log";
    Log::Start({.mFile = file, .mDebuggerOutput = false});
    Log::Warning("hands visible: {}", 2);
    Log::Stop();
    const auto contents = FileUtil::ReadAll(file).value_or("");
    Check(contents.find("WARN  hands visible: 2") != std::string::npos, "the warning line is in the file");
    Check(contents.find("log started") != std::string::npos, "the start line is in the file");
  });

  Scenario("The minimum level filters", [] {
    Captured captured;
    auto sink = captured.Attach();
    Log::Start({.mDebuggerOutput = false, .mMinimumLevel = Log::Level::Warning});
    Log::Info("hidden");
    Log::Error("shown");
    Log::Stop();
    Log::SetMinimumLevel(Log::Level::Info);
    const auto lines = captured.Lines();
    Check(lines.size() == 1 && lines[0] == "shown", "only the error got through");
  });

  // Runs last: it leaves a slow log thread behind
  Scenario("Stop waits at most 250 ms, even when a sink is slow (LG-1)", [] {
    auto slow = Log::AddSink([](const Log::Entry& entry) {
      if (entry.mText == "slow") {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    });
    Log::Start({.mDebuggerOutput = false});
    Log::Info("slow");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto start = TimeUtil::Now();
    Log::Stop();
    const auto ms = TimeUtil::ToMilliseconds(TimeUtil::Now() - start);
    Check(ms < 400, std::format("Stop returned after {:.0f} ms", ms));
    const auto exitStart = TimeUtil::Now();
    Log::Stop(Log::StopMode::ProcessExit);
    Check(TimeUtil::ToMilliseconds(TimeUtil::Now() - exitStart) < 5, "the process-exit stop doesn't wait at all");
  });

  Scenario("The log viewer's ring keeps the newest lines, Info and up only (LG-3)", [] {
    Log::LogRing ring {3, false};
    const auto add = [&ring](Log::Level level, std::string_view text) {
      ring.Add(Log::Entry {.mFileTime = 0, .mThreadID = 1, .mLevel = level, .mText = text});
    };
    add(Log::Level::Info, "one");
    add(Log::Level::Debug, "hover detail");
    add(Log::Level::Warning, "two");
    Check(ring.Lines().size() == 2, "Debug lines stay out of it");
    const auto before = ring.Version();
    add(Log::Level::Info, "three");
    add(Log::Level::Error, "four");
    Check(ring.Version() != before, "its version changes when a line is added (the viewer redraws only then)");
    const auto lines = ring.Lines();
    Check(lines.size() == 3, std::format("at most its capacity ({} lines)", lines.size()));
    Check(
      lines.size() == 3 && lines[0].mText.ends_with("two") && lines[1].mText.ends_with("three") && lines[2].mText.ends_with("four"),
      "oldest first; the oldest dropped off");
    Check(ring.Dropped() == 1, "it counts what dropped off");
    Check(ring.Text().find("WARN") != std::string::npos && ring.Text().ends_with("four\r\n"), "as text: one formatted line each");

    // Collecting from the log thread, and outliving nothing
    {
      Log::LogRing live;
      Log::Start({.mDebuggerOutput = false});
      for (int i = 0; i < 1500; ++i) {
        Log::Info("line {}", i);
        if (i % 100 == 99) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));// the log queue (256) drains
        }
      }
      Log::Stop();
      const auto kept = live.Lines();
      Check(kept.size() == Log::LogRing::DefaultCapacity, std::format("{} lines kept of 1500 (bounded)", kept.size()));
      Check(!kept.empty() && kept.back().mText.ends_with("line 1499"), "the newest line is last");
    }
  });

  return Finish();
}
