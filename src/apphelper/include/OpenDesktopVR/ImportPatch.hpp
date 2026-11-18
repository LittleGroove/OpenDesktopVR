// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Points a function the process's modules import from user32 at a
// replacement, by rewriting their import address tables (regular and
// delay-load), and puts it back exactly (ADR-031).
//
// - **Only data changes:** pointers in the import tables change; no code is
//   patched, no thread is suspended, and no memory is made executable, so it
//   works in apps that forbid changing code (Arbitrary Code Guard).
// - **Which modules:** every module loaded now, except `skip` (the helper)
//   and the system's own (under the Windows folder). Calls that don't go
//   through an import table (GetProcAddress) aren't caught.
// - **No allocation:** fixed-size, for use inside another app. Every slot is
//   swapped with a compare-exchange, so a slot someone else changed since is
//   left alone, both ways. A module unloaded since is skipped on Restore.
// - Not thread-safe: one Apply or Restore at a time.
#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>

namespace OpenDesktopVR::AppHelper {

class ImportPatch {
 public:
  static constexpr std::size_t MaxSlots = 64;

  // `functionName`, exported by user32.dll as `original`. Slots already
  // pointing at `replacement` are kept. Returns how many slots now point at
  // it (all calls so far).
  std::size_t Apply(const char* functionName, void* original, void* replacement, HMODULE skip) noexcept;
  // Puts back every slot that still points at the replacement. Returns how
  // many it put back.
  std::size_t Restore() noexcept;

  std::size_t Count() const noexcept {
    return mCount.load(std::memory_order_acquire);
  }

  // Internal: called by the module walk
  void Patch(HMODULE module, void** slot) noexcept;
  void* Replacement() const noexcept {
    return mReplacement;
  }

 private:
  struct Slot {
    HMODULE mModule {nullptr};
    void** mAddress {nullptr};
    void* mOriginal {nullptr};
  };

  bool Has(void** slot) const noexcept;

  std::array<Slot, MaxSlots> mSlots {};
  std::atomic<std::size_t> mCount {0};
  void* mReplacement {nullptr};
};

}// namespace OpenDesktopVR::AppHelper
