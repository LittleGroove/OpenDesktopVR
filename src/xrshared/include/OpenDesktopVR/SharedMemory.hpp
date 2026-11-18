// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// A named shared-memory mapping. Only the host creates; layers and tools
// only open (SY-8). "Can't create" and "can't open" are reported states,
// never exceptions: an elevated game holding the name is a normal situation
// the host explains to the user.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace OpenDesktopVR {

class SharedMemory {
 public:
  enum class Status {
    Ok,
    NotFound,     // nobody created it (no host running)
    AlreadyExists,// Create: another process holds the name
    AccessDenied, // usually an elevated process holds the name
    TooSmall,     // Open: the existing block is smaller than expected
    Failed,
  };

  SharedMemory() = default;
  ~SharedMemory();
  SharedMemory(SharedMemory&& other) noexcept;
  SharedMemory& operator=(SharedMemory&& other) noexcept;
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

  // Host side: creates a new zero-filled block; fails if the name exists
  static SharedMemory Create(std::wstring_view name, std::size_t size, Status& status) noexcept;
  // Layer and tool side
  static SharedMemory Open(std::wstring_view name, std::size_t size, Status& status) noexcept;

  bool IsValid() const noexcept {
    return mView != nullptr;
  }
  void* Data() const noexcept {
    return mView;
  }
  template <class T>
  T* As() const noexcept {
    return static_cast<T*>(mView);
  }
  std::size_t Size() const noexcept {
    return mSize;
  }

  static const char* StatusName(Status status) noexcept;

 private:
  void Reset() noexcept;
  void* mMapping {nullptr};// HANDLE
  void* mView {nullptr};
  std::size_t mSize {0};
};

// The kernel object name for `object`: Local\OpenDesktopVR.v<abi>.<object>,
// or Local\OpenDesktopVR.<instance>.v<abi>.<object> when the environment
// variable OPENDESKTOPVR_INSTANCE is set (tests run beside a real host).
std::wstring SharedObjectName(std::wstring_view object, unsigned abiVersion);

}// namespace OpenDesktopVR
