// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Sequence locks over shared memory (SY-6). One writer per section; readers
// never block it. A reader that sees a write in progress, or a counter that
// changed while it copied, gives up and keeps its previous copy.
//
// The counter is a plain uint64_t in the shared struct (so the struct stays
// trivially copyable); it's accessed through std::atomic_ref, which is
// address-free for lock-free 8-byte atomics and so works across processes.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace OpenDesktopVR::Seqlock {

static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);

// Runs `write` with the section marked "being written"
template <class Write>
void WriteSection(uint64_t& sequence, Write&& write) {
  std::atomic_ref<uint64_t> seq {sequence};
  const auto start = seq.load(std::memory_order_relaxed);
  seq.store(start + 1, std::memory_order_relaxed);// odd: writing
  std::atomic_thread_fence(std::memory_order_release);
  write();
  seq.store(start + 2, std::memory_order_release);// even: stable
}

// Copies `source` into `destination` if no write overlapped the copy.
// `attempts` bounds the retries; false means "keep your previous copy".
template <class T>
bool ReadSection(const T& source, const uint64_t& sequence, T& destination, int attempts = 3) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  // atomic_ref needs a non-const referent; this side only loads
  std::atomic_ref<uint64_t> seq {const_cast<uint64_t&>(sequence)};
  for (int i = 0; i < attempts; ++i) {
    const auto before = seq.load(std::memory_order_acquire);
    if (before & 1) {
      continue;// a write is in progress
    }
    std::memcpy(&destination, &source, sizeof(T));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) == before) {
      return true;
    }
  }
  return false;
}

}// namespace OpenDesktopVR::Seqlock
