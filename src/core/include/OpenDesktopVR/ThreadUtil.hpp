// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Threading helpers:
// - naming threads (shows in debuggers and crash dumps);
// - the catch-all every background thread and callback runs under (TH-8);
// - mutexes that detect re-entrant and nested-upgrade locking (TH-2).
#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace OpenDesktopVR::ThreadUtil {

void SetCurrentThreadName(std::wstring_view name) noexcept;

// What the in-flight exception was, for logs: std::exception::what(),
// a Windows Runtime error's HRESULT and message, or "unknown exception".
// Call only inside a catch block.
std::string DescribeCurrentException() noexcept;

// Logs an exception that escaped `where`. Defined in Log.cpp.
void ReportEscapedException(std::string_view where) noexcept;

// Runs `body`; any exception, including Windows Runtime errors, is logged
// and swallowed (doc 02, rule 8). Returns false if one escaped.
template <class Body>
bool CatchAll(std::string_view where, Body&& body) noexcept {
  try {
    std::forward<Body>(body)();
    return true;
  } catch (...) {
    ReportEscapedException(where);
    return false;
  }
}

// ---- Lock checking (TH-2) -------------------------------------------------
//
// Every CheckedMutex / CheckedSharedMutex records itself in a small
// per-thread list while held. Taking one the thread already holds (in any
// mode) is a violation: re-entrancy, or a shared-to-exclusive upgrade, both
// of which deadlock. On by default in debug builds.

namespace LockCheck {
using ViolationHandler = void (*)(const char* what, const void* mutex);

bool Enabled() noexcept;
void SetEnabled(bool enabled) noexcept;
// The default handler logs, then breaks into the debugger (debug builds)
void SetViolationHandler(ViolationHandler handler) noexcept;

// How many checked locks this thread holds right now
std::size_t HeldByThisThread() noexcept;

namespace Detail {
void OnAcquire(const void* mutex) noexcept;
void OnRelease(const void* mutex) noexcept;
bool IsHeldByThisThread(const void* mutex) noexcept;
}// namespace Detail
}// namespace LockCheck

class CheckedMutex {
 public:
  void lock() {
    LockCheck::Detail::OnAcquire(this);
    mMutex.lock();
  }
  bool try_lock() {
    if (LockCheck::Enabled() && LockCheck::Detail::IsHeldByThisThread(this)) {
      LockCheck::Detail::OnAcquire(this);// reports the violation
      return false;
    }
    if (!mMutex.try_lock()) {
      return false;
    }
    LockCheck::Detail::OnAcquire(this);
    return true;
  }
  void unlock() {
    mMutex.unlock();
    LockCheck::Detail::OnRelease(this);
  }

 private:
  std::mutex mMutex;
};

class CheckedSharedMutex {
 public:
  void lock() {
    LockCheck::Detail::OnAcquire(this);
    mMutex.lock();
  }
  void unlock() {
    mMutex.unlock();
    LockCheck::Detail::OnRelease(this);
  }
  void lock_shared() {
    LockCheck::Detail::OnAcquire(this);
    mMutex.lock_shared();
  }
  void unlock_shared() {
    mMutex.unlock_shared();
    LockCheck::Detail::OnRelease(this);
  }

 private:
  std::shared_mutex mMutex;
};

}// namespace OpenDesktopVR::ThreadUtil
