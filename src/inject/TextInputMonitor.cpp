// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
// Changed in M5b (ADR-036): checks asked for with Poke() are kept, several
// at once (up to 8), each with its own "report" flag; it kept only the
// earliest, so a tap's 400 ms check was lost behind its 80 ms one.
#include <OpenDesktopVR/TextInputMonitor.hpp>

#include <Windows.h>

#include <oleacc.h>

#pragma comment(lib, "oleacc.lib")

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace OpenDesktopVR {

namespace {

constexpr UINT SetWindowMessage = WM_APP + 1;
constexpr UINT PokeMessage = WM_APP + 2;

// How long to wait for the app to answer; a busy app is asked again later
constexpr UINT QueryTimeoutMilliseconds = 200;

std::optional<RECT> CaretFromAccessible(HWND window) {
  // Like AccessibleObjectFromWindow(), but without waiting forever for an app
  // that has stopped responding
  DWORD_PTR result {};
  if (!SendMessageTimeoutW(
        window,
        WM_GETOBJECT,
        0,
        static_cast<LPARAM>(OBJID_CARET),
        SMTO_NORMAL | SMTO_ABORTIFHUNG,
        QueryTimeoutMilliseconds,
        &result)) {
    return std::nullopt;
  }
  IAccessible* accessible {};
  const auto lresult = static_cast<LRESULT>(result);
  if (lresult > 0) {
    if (FAILED(ObjectFromLresult(
          lresult, IID_IAccessible, 0, reinterpret_cast<void**>(&accessible)))) {
      return std::nullopt;
    }
  } else if (FAILED(CreateStdAccessibleObject(
               window,
               OBJID_CARET,
               IID_IAccessible,
               reinterpret_cast<void**>(&accessible)))) {
    return std::nullopt;
  }
  if (!accessible) {
    return std::nullopt;
  }

  std::optional<RECT> ret;
  VARIANT self {};
  self.vt = VT_I4;
  self.lVal = CHILDID_SELF;
  VARIANT state {};
  if (
    SUCCEEDED(accessible->get_accState(self, &state)) && state.vt == VT_I4
    && !(state.lVal & STATE_SYSTEM_INVISIBLE)) {
    long x {}, y {}, width {}, height {};
    if (
      accessible->accLocation(&x, &y, &width, &height, self) == S_OK
      && height > 0) {
      ret = RECT {x, y, x + std::max(width, 1L), y + height};
    }
  }
  accessible->Release();
  return ret;
}

}// namespace

bool TextInputMonitor::State::operator==(const State& other) const {
  if (mTyping != other.mTyping || mCaret.has_value() != other.mCaret.has_value()) {
    return false;
  }
  if (!mCaret) {
    return true;
  }
  const auto& a = *mCaret;
  const auto& b = *other.mCaret;
  return a.left == b.left && a.top == b.top && a.right == b.right
    && a.bottom == b.bottom;
}

TextInputMonitor::State TextInputMonitor::Query(HWND topLevel) {
  State ret;
  if (!(topLevel && IsWindow(topLevel)) || IsHungAppWindow(topLevel)) {
    return ret;
  }
  const auto thread = GetWindowThreadProcessId(topLevel, nullptr);
  GUITHREADINFO info {.cbSize = sizeof(GUITHREADINFO)};
  if (!GetGUIThreadInfo(thread, &info)) {
    info = {.cbSize = sizeof(GUITHREADINFO)};
  }

  // A real Windows caret: Win32 text controls, and Firefox
  if (info.hwndCaret && GetAncestor(info.hwndCaret, GA_ROOT) == topLevel) {
    RECT caret = info.rcCaret;
    MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT*>(&caret), 2);
    ret.mTyping = true;
    ret.mCaret = caret;
    return ret;
  }

  // Chromium's accessibility caret. Also the request that makes Firefox start
  // creating a real caret.
  const auto focus
    = (info.hwndFocus && GetAncestor(info.hwndFocus, GA_ROOT) == topLevel)
    ? info.hwndFocus
    : topLevel;
  if (const auto caret = CaretFromAccessible(focus)) {
    ret.mTyping = true;
    ret.mCaret = caret;
  }
  return ret;
}

class TextInputMonitor::Impl final {
 public:
  explicit Impl(Callback callback) : mCallback(std::move(callback)) {
    std::unique_lock lock(mMutex);
    mThread = std::jthread([this]() { this->Run(); });
    mReady.wait(lock, [this]() { return mThreadID != 0; });
  }

  ~Impl() {
    PostThreadMessageW(mThreadID, WM_QUIT, 0, 0);
    mThread.join();
  }

  void SetWindow(HWND window) {
    PostThreadMessageW(
      mThreadID, SetWindowMessage, reinterpret_cast<WPARAM>(window), 0);
  }

  void Poke(std::chrono::milliseconds delay, bool report) {
    PostThreadMessageW(
      mThreadID,
      PokeMessage,
      static_cast<WPARAM>(delay.count()),
      report ? 1 : 0);
  }

  State GetState() const {
    std::unique_lock lock(mMutex);
    return mState;
  }

 private:
  using Clock = std::chrono::steady_clock;

  Callback mCallback;
  mutable std::mutex mMutex;
  std::condition_variable mReady;
  DWORD mThreadID {};
  State mState;// guarded by mMutex
  std::jthread mThread;

  // Only used on the monitor thread
  HWND mWindow {};
  HWINEVENTHOOK mHook {};
  UINT_PTR mTimer {};
  std::optional<Clock::time_point> mDue;
  // Checks asked for with Poke(), soonest first (at most MaxPokes)
  struct Pending {
    Clock::time_point mDue;
    bool mReport {false};
  };
  static constexpr std::size_t MaxPokes = 8;
  std::vector<Pending> mPokes;
  std::optional<Clock::time_point> mNotTypingSince;
  // The next evaluation calls back even if nothing changed
  bool mReportNext {false};

  static thread_local Impl* tCurrent;

  void Run() {
    SetThreadDescription(GetCurrentThread(), L"Text Input Monitor");
    tCurrent = this;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MSG msg {};
    // Create this thread's message queue before anyone posts to it
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
      std::unique_lock lock(mMutex);
      mThreadID = GetCurrentThreadId();
    }
    mReady.notify_all();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.hwnd) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        continue;
      }
      switch (msg.message) {
        case SetWindowMessage:
          this->OnSetWindow(reinterpret_cast<HWND>(msg.wParam));
          break;
        case PokeMessage:
          this->AddPoke(std::chrono::milliseconds(msg.wParam), msg.lParam != 0);
          break;
        case WM_TIMER:
          if (msg.wParam == mTimer) {
            KillTimer(nullptr, mTimer);
            mTimer = 0;
            mDue.reset();
            this->TakeDuePokes();
            this->Evaluate();
            this->ArmNextPoke();
          }
          break;
      }
    }

    this->Unhook();
    if (mTimer) {
      KillTimer(nullptr, mTimer);
    }
    tCurrent = nullptr;
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
  }

  void Unhook() {
    if (mHook) {
      UnhookWinEvent(mHook);
      mHook = {};
    }
  }

  void OnSetWindow(HWND window) {
    if (window == mWindow) {
      this->Schedule(std::chrono::milliseconds(0));
      return;
    }
    this->Unhook();
    mWindow = window;
    mNotTypingSince.reset();
    this->SetState({});
    if (!window) {
      return;
    }
    DWORD processID {};
    GetWindowThreadProcessId(window, &processID);
    if (!processID) {
      return;
    }
    // Only this process's events; delivered to this thread's message queue
    mHook = SetWinEventHook(
      EVENT_OBJECT_CREATE,
      EVENT_OBJECT_LOCATIONCHANGE,
      nullptr,
      &Impl::OnWinEvent,
      processID,
      0,
      WINEVENT_OUTOFCONTEXT);
    // Also asks Firefox for its caret object, so it creates one from now on
    this->Schedule(std::chrono::milliseconds(0));
  }

  static void CALLBACK OnWinEvent(
    HWINEVENTHOOK,
    DWORD,
    HWND hwnd,
    LONG object,
    LONG,
    DWORD,
    DWORD) {
    if (object != OBJID_CARET || !tCurrent || !hwnd) {
      return;
    }
    if (GetAncestor(hwnd, GA_ROOT) != tCurrent->mWindow) {
      return;
    }
    // Several events usually come together; look once they've settled
    tCurrent->Schedule(std::chrono::milliseconds(30));
  }

  // A check asked for: kept with any others (one within 10 ms of it is the
  // same check), so each happens
  void AddPoke(std::chrono::milliseconds delay, bool report) {
    const auto due = Clock::now() + delay;
    for (auto& poke: mPokes) {
      if (poke.mDue - due < std::chrono::milliseconds(10) && due - poke.mDue < std::chrono::milliseconds(10)) {
        poke.mDue = std::min(poke.mDue, due);
        poke.mReport = poke.mReport || report;
        this->Schedule(std::chrono::duration_cast<std::chrono::milliseconds>(poke.mDue - Clock::now()));
        return;
      }
    }
    if (mPokes.size() >= MaxPokes) {
      // Full: the latest one also reports, so nothing is missed
      mPokes.back().mReport = mPokes.back().mReport || report;
      return;
    }
    mPokes.push_back({due, report});
    std::ranges::sort(mPokes, {}, &Pending::mDue);
    this->ArmNextPoke();
  }

  // The checks due now: a report from any of them goes with this evaluation
  void TakeDuePokes() {
    const auto now = Clock::now() + std::chrono::milliseconds(1);
    while (!mPokes.empty() && mPokes.front().mDue <= now) {
      mReportNext = mReportNext || mPokes.front().mReport;
      mPokes.erase(mPokes.begin());
    }
  }

  void ArmNextPoke() {
    if (mPokes.empty()) {
      return;
    }
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(mPokes.front().mDue - Clock::now());
    this->Schedule(std::max(wait, std::chrono::milliseconds(0)));
  }

  void Schedule(std::chrono::milliseconds delay) {
    const auto due = Clock::now() + delay;
    if (mDue && *mDue <= due) {
      return;
    }
    mDue = due;
    const auto ms = static_cast<UINT>(std::max<long long>(delay.count(), 1));
    mTimer = SetTimer(nullptr, mTimer, ms, nullptr);
  }

  void Evaluate() {
    if (!mWindow) {
      return;
    }
    if (!IsWindow(mWindow)) {
      this->SetState({});
      return;
    }
    const auto state = Query(mWindow);
    if (state.mTyping) {
      mNotTypingSince.reset();
      this->SetState(state);
      return;
    }
    if (!this->GetState().mTyping) {
      this->SetState(state);
      return;
    }
    // Typing until 'not typing' has lasted a moment
    const auto now = Clock::now();
    if (!mNotTypingSince) {
      mNotTypingSince = now;
    }
    const auto elapsed = now - *mNotTypingSince;
    if (elapsed >= NotTypingDelay) {
      mNotTypingSince.reset();
      this->SetState(state);
      return;
    }
    this->Schedule(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        NotTypingDelay - elapsed)
      + std::chrono::milliseconds(1));
  }

  void SetState(const State& state) {
    const bool report = std::exchange(mReportNext, false);
    {
      std::unique_lock lock(mMutex);
      if (state == mState && !report) {
        return;
      }
      mState = state;
    }
    if (mCallback) {
      mCallback(mWindow, state);
    }
  }
};

thread_local TextInputMonitor::Impl* TextInputMonitor::Impl::tCurrent {};

TextInputMonitor::TextInputMonitor(Callback callback)
  : mImpl(std::make_unique<Impl>(std::move(callback))) {
}

TextInputMonitor::~TextInputMonitor() = default;

void TextInputMonitor::SetWindow(HWND topLevel) {
  mImpl->SetWindow(topLevel);
}

void TextInputMonitor::Poke(std::chrono::milliseconds delay, bool report) {
  mImpl->Poke(delay, report);
}

TextInputMonitor::State TextInputMonitor::GetState() const {
  return mImpl->GetState();
}

}// namespace OpenDesktopVR
