// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <Windows.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>

namespace OpenDesktopVR::ThreadUtil {

void SetCurrentThreadName(std::wstring_view name) noexcept {
  try {
    const std::wstring terminated {name};
    SetThreadDescription(GetCurrentThread(), terminated.c_str());
  } catch (...) {
  }
}

std::string DescribeCurrentException() noexcept {
  try {
    throw;
  } catch (const winrt::hresult_error& e) {
    try {
      return std::format(
        "Windows Runtime error 0x{:08X}: {}",
        static_cast<std::uint32_t>(e.code().value),
        StringUtil::Narrow(e.message()));
    } catch (...) {
      return "Windows Runtime error";
    }
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "unknown exception";
  }
}

namespace LockCheck {

namespace {

#ifdef NDEBUG
constexpr bool DefaultEnabled = false;
#else
constexpr bool DefaultEnabled = true;
#endif

std::atomic<bool> gEnabled {DefaultEnabled};

void DefaultViolationHandler(const char* what, const void* mutex) {
  Log::Error("Lock check: {} (mutex {})", what, mutex);
#ifndef NDEBUG
  if (IsDebuggerPresent()) {
    __debugbreak();
  }
#endif
}

std::atomic<ViolationHandler> gHandler {&DefaultViolationHandler};

// Nesting deeper than this is itself a design smell; extra entries are
// simply not tracked
constexpr std::size_t MaxTracked = 16;
thread_local std::array<const void*, MaxTracked> tHeld {};
thread_local std::size_t tHeldCount = 0;

}// namespace

bool Enabled() noexcept {
  return gEnabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) noexcept {
  gEnabled.store(enabled, std::memory_order_relaxed);
}

void SetViolationHandler(ViolationHandler handler) noexcept {
  gHandler.store(handler ? handler : &DefaultViolationHandler);
}

std::size_t HeldByThisThread() noexcept {
  return std::min(tHeldCount, MaxTracked);
}

namespace Detail {

bool IsHeldByThisThread(const void* mutex) noexcept {
  const auto count = std::min(tHeldCount, MaxTracked);
  return std::find(tHeld.begin(), tHeld.begin() + count, mutex) != tHeld.begin() + count;
}

void OnAcquire(const void* mutex) noexcept {
  if (!Enabled()) {
    return;
  }
  if (IsHeldByThisThread(mutex)) {
    gHandler.load()("this thread already holds this lock (re-entrant or upgrade)", mutex);
    return;
  }
  if (tHeldCount < MaxTracked) {
    tHeld[tHeldCount] = mutex;
  }
  ++tHeldCount;
}

void OnRelease(const void* mutex) noexcept {
  if (!Enabled() || tHeldCount == 0) {
    return;
  }
  const auto count = std::min(tHeldCount, MaxTracked);
  const auto it = std::find(tHeld.begin(), tHeld.begin() + count, mutex);
  if (it != tHeld.begin() + count) {
    std::move(it + 1, tHeld.begin() + count, it);
    --tHeldCount;
  } else if (tHeldCount > MaxTracked) {
    --tHeldCount;// one of the untracked overflow entries
  }
  // Otherwise: the second release after a reported violation; nothing to undo
}

}// namespace Detail
}// namespace LockCheck
}// namespace OpenDesktopVR::ThreadUtil
