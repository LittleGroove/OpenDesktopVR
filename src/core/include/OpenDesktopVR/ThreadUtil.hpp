// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Threading helpers:
// - naming threads (shows in debuggers and crash dumps);
// - the catch-all every background thread and callback runs under (TH-8);
// - mutexes that detect re-entrant and nested-upgrade locking (TH-2);
// - background tasks the UI thread can start and check without waiting
//   (ADR-020).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>

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

// ---- Background tasks (ADR-020) ------------------------------------------
//
// Slow work the UI thread mustn't do itself (window enumeration, disk I/O),
// run on a worker thread and checked without waiting. Dropping a task never
// waits for its worker (a std::async future would): the worker finishes on
// its own and its result is thrown away.
template <class T>
class BackgroundTask {
 public:
  BackgroundTask() = default;

  // `work` returns a T and runs under CatchAll; if it throws, the task
  // finishes with no result. Throws std::system_error if no thread starts.
  template <class Work>
  static BackgroundTask Start(std::wstring_view name, Work&& work) {
    BackgroundTask task;
    task.mState = std::make_shared<State>();
    std::thread(
      [state = task.mState, name = std::wstring {name}, work = std::forward<Work>(work)]() mutable {
        SetCurrentThreadName(name);
        CatchAll("background task", [&] { state->mValue.emplace(work()); });
        state->mDone.store(true, std::memory_order_release);
      })
      .detach();
    return task;
  }

  // Started and not yet taken
  bool Started() const noexcept {
    return mState != nullptr;
  }
  bool Running() const noexcept {
    return mState && !mState->mDone.load(std::memory_order_acquire);
  }
  // Once the worker is done: its result (nullopt if it threw), handed over
  // once, after which the task is empty. nullopt while it's running.
  std::optional<T> Take() {
    if (!mState || !mState->mDone.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    const auto state = std::move(mState);
    return std::move(state->mValue);
  }

 private:
  struct State {
    std::atomic<bool> mDone {false};
    std::optional<T> mValue;
  };
  std::shared_ptr<State> mState;
};

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
