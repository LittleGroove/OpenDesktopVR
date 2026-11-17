// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Publisher.hpp>
#include <OpenDesktopVR/Seqlock.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace OpenDesktopVR::Host {

namespace {

// Unique per host start, so a restarted host never repeats a generation
// a layer has already seen (SY-4)
uint64_t FirstGeneration() noexcept {
  LARGE_INTEGER counter {};
  QueryPerformanceCounter(&counter);
  return ((static_cast<uint64_t>(counter.QuadPart) << 16) ^ GetCurrentProcessId()) | 1;
}

}// namespace

std::optional<Publisher> Publisher::Create(SharedMemory::Status& status, void* wakeEvent, void* trackingEvent) noexcept {
  auto memory = SharedMemory::Create(
    SharedObjectName(SharedAbi::BlockObject, SharedAbi::AbiVersion), sizeof(SharedAbi::Block), status);
  if (!memory.IsValid()) {
    return std::nullopt;
  }
  auto& block = *memory.As<SharedAbi::Block>();
  // The table and status start empty (the mapping is zero-filled); the header
  // goes last, with its magic, so a layer never sees a half-written header
  block.mHeader.mAbiVersion = SharedAbi::AbiVersion;
  block.mHeader.mBlockSize = sizeof(SharedAbi::Block);
  block.mHeader.mPanelEntrySize = sizeof(SharedAbi::PanelEntry);
  block.mHeader.mStatusSize = sizeof(SharedAbi::LayerStatus);
  block.mHeader.mHostProcessID = GetCurrentProcessId();
  block.mHeader.mWakeEvent = reinterpret_cast<uint64_t>(wakeEvent);
  block.mHeader.mTrackingEvent = reinterpret_cast<uint64_t>(trackingEvent);
  std::atomic_ref<uint64_t> {block.mHeader.mSessionGeneration}.store(FirstGeneration());
  std::atomic_ref<uint32_t> {block.mHeader.mMagic}.store(SharedAbi::Magic, std::memory_order_release);
  return Publisher {std::move(memory)};
}

uint64_t Publisher::Generation() const noexcept {
  return std::atomic_ref<uint64_t> {Block().mHeader.mSessionGeneration}.load();
}

void Publisher::WriteRecenterCount(uint64_t count) noexcept {
  std::atomic_ref<uint64_t> {Block().mHeader.mRecenterCount}.store(count, std::memory_order_relaxed);
}

void Publisher::BumpGeneration() noexcept {
  std::atomic_ref<uint64_t> {Block().mHeader.mSessionGeneration}.fetch_add(2);
}

void Publisher::PublishPanels(std::span<const SharedAbi::PanelEntry> panels) noexcept {
  auto& table = Block().mPanelTable;
  const auto count = std::min<std::size_t>(panels.size(), SharedAbi::MaxPanels);
  Seqlock::WriteSection(table.mSequence, [&] {
    table.mPanelCount = static_cast<uint32_t>(count);
    std::memcpy(table.mPanels, panels.data(), count * sizeof(SharedAbi::PanelEntry));
  });
}

bool Publisher::HasViewers(uint64_t windowMilliseconds) const noexcept {
  const auto beat = std::atomic_ref<uint64_t> {Block().mHeader.mViewerHeartbeat}.load(std::memory_order_relaxed);
  return beat != 0 && GetTickCount64() - beat < windowMilliseconds;
}

uint64_t Publisher::TrackingWritten() const noexcept {
  return std::atomic_ref<uint64_t> {Block().mTracking.mWritten}.load(std::memory_order_acquire);
}

bool Publisher::ReadTrackingSample(uint64_t index, SharedAbi::TrackingSample& out) const noexcept {
  if (index == 0) {
    return false;
  }
  const auto& slot = Block().mTracking.mSlots[index % SharedAbi::TrackingSlots];
  // A slot rewritten with a newer sample (the reader fell behind) is skipped
  return Seqlock::ReadSection(slot, slot.mSequence, out) && out.mIndex == index;
}

void Publisher::WriteHandState(const SharedAbi::HandState& state) noexcept {
  auto& section = Block().mHands;
  Seqlock::WriteSection(section.mSequence, [&] {
    auto copy = state;
    copy.mSequence = section.mSequence;
    std::memcpy(&section, &copy, sizeof(section));
  });
}

std::optional<SharedAbi::LayerStatus> Publisher::ReadStatus() const noexcept {
  SharedAbi::LayerStatus status {};
  if (!Seqlock::ReadSection(Block().mStatus, Block().mStatus.mSequence, status)) {
    return std::nullopt;
  }
  return status;
}

}// namespace OpenDesktopVR::Host
