// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The shared-memory ABI: fixed layout and version checks (SY-7), host-only
// creation (SY-8), and seqlocks that never block the writer and never hand
// a reader a torn copy (SY-6).

#include "check.hpp"

#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/Seqlock.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>

#include <Windows.h>

#include <atomic>
#include <format>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;

namespace {

std::wstring UniqueName(std::wstring_view what) {
  return std::format(L"Local\\OpenDesktopVR.test-{}.{}", GetCurrentProcessId(), what);
}

}// namespace

int main() {
  Scenario("Layout and version checks (SY-7)", [] {
    SharedAbi::Header header {};
    header.mMagic = SharedAbi::Magic;
    header.mAbiVersion = SharedAbi::AbiVersion;
    header.mBlockSize = sizeof(SharedAbi::Block);
    header.mPanelEntrySize = sizeof(SharedAbi::PanelEntry);
    header.mStatusSize = sizeof(SharedAbi::LayerStatus);
    Check(SharedAbi::IsCompatible(header), "a matching header is compatible");
    auto other = header;
    other.mAbiVersion += 1;
    Check(!SharedAbi::IsCompatible(other), "another ABI version is not");
    other = header;
    other.mPanelEntrySize += 8;
    Check(!SharedAbi::IsCompatible(other), "another entry size is not");
    other = header;
    other.mMagic = 0;
    Check(!SharedAbi::IsCompatible(other), "garbage is not");
  });

  Scenario("Only one creator; others open (SY-8)", [] {
    const auto name = UniqueName(L"Create");
    SharedMemory::Status status {};
    auto host = SharedMemory::Create(name, sizeof(SharedAbi::Block), status);
    Check(host.IsValid() && status == SharedMemory::Status::Ok, "the first create succeeds");
    host.As<SharedAbi::Block>()->mHeader.mHostProcessID = 1234;

    auto squatter = SharedMemory::Create(name, sizeof(SharedAbi::Block), status);
    Check(!squatter.IsValid() && status == SharedMemory::Status::AlreadyExists, "a second create is refused, not adopted");

    auto layer = SharedMemory::Open(name, sizeof(SharedAbi::Block), status);
    Check(layer.IsValid() && layer.As<SharedAbi::Block>()->mHeader.mHostProcessID == 1234, "open sees the host's data");

    auto bigger = SharedMemory::Open(name, sizeof(SharedAbi::Block) + 65536, status);
    Check(!bigger.IsValid() && status == SharedMemory::Status::TooSmall, "a block smaller than expected is refused");

    auto missing = SharedMemory::Open(UniqueName(L"Nobody"), sizeof(SharedAbi::Block), status);
    Check(!missing.IsValid() && status == SharedMemory::Status::NotFound, "no host: 'not found', no exception");
  });

  Scenario("Object names: versioned, session-local, test instances apart", [] {
    SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", nullptr);
    Check(SharedObjectName(L"Shared", 1) == L"Local\\OpenDesktopVR.v1.Shared", "the product's name");
    SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", L"ci");
    Check(SharedObjectName(L"Shared", 1) == L"Local\\OpenDesktopVR.ci.v1.Shared", "a test instance's name");
    SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", nullptr);
  });

  Scenario("Seqlock: readers never see a torn table, and never stall the writer (SY-6)", [] {
    auto table = std::make_unique<SharedAbi::PanelTable>();
    std::atomic<bool> stop {false};
    std::atomic<uint64_t> writes {0};
    std::thread writer([&] {
      for (uint64_t n = 1; !stop.load(std::memory_order_relaxed); ++n) {
        Seqlock::WriteSection(table->mSequence, [&] {
          table->mPanelCount = static_cast<uint32_t>(n);
          for (auto& panel: table->mPanels) {
            panel.mContentVersion = n;
            panel.mFenceValue = n;
          }
        });
        writes.store(n, std::memory_order_relaxed);
      }
    });
    int good = 0;
    int torn = 0;
    int refused = 0;
    auto copy = std::make_unique<SharedAbi::PanelTable>();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
      if (!Seqlock::ReadSection(*table, table->mSequence, *copy)) {
        ++refused;
        continue;
      }
      bool consistent = true;
      for (const auto& panel: copy->mPanels) {
        consistent = consistent && panel.mContentVersion == copy->mPanelCount && panel.mFenceValue == copy->mPanelCount;
      }
      consistent ? ++good : ++torn;
    }
    stop = true;
    writer.join();
    Check(torn == 0, std::format("no torn copies accepted ({} good, {} refused and kept previous)", good, refused));
    Check(good > 100, "readers mostly succeed");
    Check(writes.load() > 10000, std::format("the writer never waited for readers ({} writes)", writes.load()));
  });

  return Finish();
}
