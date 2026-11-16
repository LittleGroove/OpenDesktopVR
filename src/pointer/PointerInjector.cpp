// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/AppHelperProtocol.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace OpenDesktopVR::Pointer {

using namespace std::chrono_literals;
using HandInteraction::CursorUpdate;
using HandInteraction::TouchState;
namespace Protocol = AppHelperProtocol;

namespace {

// A client point as mouse messages carry it: signed 16-bit x and y
LPARAM PackClient(POINT point) noexcept {
  return Protocol::PackPoint(point.x, point.y);
}

// The deepest child under a point of `top`'s client area that would get a
// real click: visible, enabled, not transparent (doc 06 "Targeting")
HWND DeepestChild(HWND top, POINT client) noexcept {
  HWND parent = top;
  POINT point = client;
  for (int depth = 0; depth < 32; ++depth) {
    const HWND child = ChildWindowFromPointEx(parent, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
    if (!child || child == parent) {
      break;
    }
    MapWindowPoints(parent, child, &point, 1);
    parent = child;
  }
  return parent;
}

// Runs in the target window's DPI context, so its client area and points are
// in its own units (a DPI-unaware app scaled by Windows sees its own)
class WindowDpi {
 public:
  explicit WindowDpi(HWND window) noexcept
    : mPrevious(SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window))) {
  }
  ~WindowDpi() {
    if (mPrevious) {
      SetThreadDpiAwarenessContext(mPrevious);
    }
  }
  WindowDpi(const WindowDpi&) = delete;
  WindowDpi& operator=(const WindowDpi&) = delete;

 private:
  DPI_AWARENESS_CONTEXT mPrevious;
};

}// namespace

POINT ClientPoint(const Target& target, float u, float v, LONG clientWidth, LONG clientHeight) noexcept {
  const auto fraction = [](LONG start, LONG size, float along, LONG total) {
    if (total <= 0) {
      return 0.5;
    }
    const double pixel = start + (static_cast<double>(std::clamp(along, 0.0f, 1.0f)) * size);
    return std::clamp(pixel / total, 0.0, 1.0);
  };
  const double fx = fraction(target.mShown.left, target.mShown.right - target.mShown.left, u, target.mClient.cx);
  const double fy = fraction(target.mShown.top, target.mShown.bottom - target.mShown.top, v, target.mClient.cy);
  // Inside the client area: its last pixel at most
  return {
    std::clamp(static_cast<LONG>(std::floor(fx * clientWidth)), LONG {0}, std::max(LONG {0}, clientWidth - 1)),
    std::clamp(static_cast<LONG>(std::floor(fy * clientHeight)), LONG {0}, std::max(LONG {0}, clientHeight - 1)),
  };
}

namespace {

enum class Kind { Move, Down, Up, Wheel, Leave };

struct Event {
  Kind mKind {Kind::Move};
  Target mTarget;
  float mU {0};
  float mV {0};
  int32_t mWheel {0};
  bool mDrag {false};// a move with the button held
  TimeUtil::TimePoint mQueued {};
};

struct Shared {
  Options mOptions;
  UINT mCommandMessage {RegisterWindowMessageW(Protocol::CommandMessageName)};
  mutable std::mutex mStatsMutex;
  Stats mStats;
  template <class Change>
  void Count(Change&& change) {
    std::scoped_lock lock(mStatsMutex);
    change(mStats);
  }
};

class Worker {
 public:
  Worker(Shared& shared, HWND window) : mShared(shared), mWindow(window) {
    mThread = std::thread([this] {
      ThreadUtil::SetCurrentThreadName(L"Input");
      ThreadUtil::CatchAll("input worker", [this] { Main(); });
    });
  }
  ~Worker() {
    {
      std::scoped_lock lock(mMutex);
      mStop = true;
    }
    SetEvent(mWake);
    if (mThread.joinable()) {
      mThread.join();// a send in flight returns within its timeout
    }
    // INJ-4: a button still held is let go, posted (never waiting on it)
    if (mDown && mCapture && IsWindow(mCapture)) {
      PostMessageW(mCapture, WM_LBUTTONUP, 0, PackClient(mLastPoint));
      PostMessageW(mWindow, mShared.mCommandMessage, static_cast<WPARAM>(Protocol::Command::EndInjection), 0);
      mShared.Count([](auto& stats) { ++stats.mPostedReleases; });
    }
    CloseHandle(mWake);
  }
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void Enqueue(Event event) {
    event.mQueued = TimeUtil::Now();
    bool merged = false;
    bool dropped = false;
    {
      std::scoped_lock lock(mMutex);
      // INJ-1: a newer move replaces a queued one; nothing else merges
      if (event.mKind == Kind::Move && !mQueue.empty() && mQueue.back().mKind == Kind::Move
          && mQueue.back().mDrag == event.mDrag) {
        event.mQueued = mQueue.back().mQueued;
        mQueue.back() = event;
        merged = true;
      } else if (mQueue.size() >= mShared.mOptions.mQueueCapacity) {
        // TH-5: bounded. Moves go first; buttons only past twice the size
        const auto move = std::find_if(mQueue.begin(), mQueue.end(), [](const Event& e) { return e.mKind == Kind::Move; });
        if (move != mQueue.end()) {
          mQueue.erase(move);
          mQueue.push_back(event);
          merged = true;
        } else if (event.mKind != Kind::Move && mQueue.size() < 2 * mShared.mOptions.mQueueCapacity) {
          mQueue.push_back(event);
        } else {
          dropped = true;
        }
      } else {
        mQueue.push_back(event);
      }
    }
    if (merged) {
      mShared.Count([](auto& stats) { ++stats.mMovesMerged; });
    }
    if (dropped) {
      mShared.Count([](auto& stats) { ++stats.mDropped; });
    }
    SetEvent(mWake);
  }

  HWND Window() const noexcept {
    return mWindow;
  }

 private:
  void Main() {
    for (;;) {
      Event event;
      {
        std::unique_lock lock(mMutex);
        if (mStop) {
          return;
        }
        if (mQueue.empty()) {
          lock.unlock();
          WaitForSingleObject(mWake, INFINITE);
          continue;
        }
        // Hover at most 30 a second; the newest waits, merging newer ones.
        // Something else behind it never waits for it: a hover move is
        // dropped then (a press makes its own move to the exact point).
        const auto& front = mQueue.front();
        if (front.mKind == Kind::Move && !front.mDrag && !mDown && mQueue.size() > 1) {
          mQueue.pop_front();
          lock.unlock();
          mShared.Count([](auto& stats) { ++stats.mMovesMerged; });
          continue;
        }
        if (front.mKind == Kind::Move && !front.mDrag && !mDown) {
          const auto now = TimeUtil::Now();
          if (now < mNextHoverAt) {
            const auto wait = std::chrono::ceil<std::chrono::milliseconds>(mNextHoverAt - now);
            lock.unlock();
            WaitForSingleObject(mWake, static_cast<DWORD>(std::max<int64_t>(1, wait.count())));
            continue;
          }
        }
        event = mQueue.front();
        mQueue.pop_front();
      }
      ThreadUtil::CatchAll("input delivery", [&] { Deliver(event); });
    }
  }

  bool Send(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(window, message, wParam, lParam, SMTO_NORMAL | SMTO_ABORTIFHUNG,
          static_cast<UINT>(mShared.mOptions.mTimeout.count()), &result)) {
      return true;
    }
    mBusy = true;
    mShared.Count([](auto& stats) { ++stats.mTimeouts; });
    const auto now = TimeUtil::Now();
    if (now >= mNextTimeoutLog) {
      mNextTimeoutLog = now + 10s;
      Log::Warning("Input: a window took more than {} ms to take a message; carrying on without it",
        mShared.mOptions.mTimeout.count());
    }
    return false;
  }

  // A window that didn't take a message in time may still be working
  // through it; until it answers again it's treated as hung (INJ-3), so
  // clicks don't pile up in it to all land at once when it recovers
  bool Responding(HWND top) {
    if (IsHungAppWindow(top)) {
      return false;
    }
    if (!mBusy) {
      return true;
    }
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(top, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 50, &result)) {
      mBusy = false;
      return true;
    }
    return false;
  }

  void Command(Protocol::Command command, LPARAM argument) {
    Send(mWindow, mShared.mCommandMessage, static_cast<WPARAM>(command), argument);
  }

  bool IsDoubleTap(HWND window, POINT screen, DWORD now) {
    const auto& options = mShared.mOptions;
    const int dx = std::max(options.mDoubleTapPixels, GetSystemMetrics(SM_CXDOUBLECLK) / 2);
    const int dy = std::max(options.mDoubleTapPixels, GetSystemMetrics(SM_CYDOUBLECLK) / 2);
    return mLastDownWindow == window && mLastDownTick != 0 && now - mLastDownTick <= GetDoubleClickTime()
      && std::abs(screen.x - mLastDownScreen.x) <= dx && std::abs(screen.y - mLastDownScreen.y) <= dy
      && (GetClassLongPtrW(window, GCL_STYLE) & CS_DBLCLKS) != 0;
  }

  void LetGo(HWND target, POINT local) {
    if (local.x != mLastPoint.x || local.y != mLastPoint.y) {
      Send(target, WM_MOUSEMOVE, MK_LBUTTON, PackClient(local));
    }
    Command(Protocol::Command::SetMouseButtons, 0);
    Send(target, WM_LBUTTONUP, 0, PackClient(local));
    mDown = false;
    mCapture = nullptr;
    mLastPoint = local;
    mShared.Count([](auto& stats) { ++stats.mReleases; });
  }

  void Deliver(const Event& event) {
    const HWND top = mWindow;
    if (!IsWindow(top)) {
      mDown = false;
      mCapture = nullptr;
      mHovered = nullptr;
      mShared.Count([](auto& stats) { ++stats.mWindowsGone; });
      return;
    }
    // INJ-3: a hung window gets its release and the end posted; the rest
    // waits for nobody
    if (!Responding(top)) {
      if ((event.mKind == Kind::Up || event.mKind == Kind::Leave) && mDown) {
        const HWND target = mCapture && IsWindow(mCapture) ? mCapture : top;
        PostMessageW(top, mShared.mCommandMessage, static_cast<WPARAM>(Protocol::Command::SetMouseButtons), 0);
        PostMessageW(target, WM_LBUTTONUP, 0, PackClient(mLastPoint));
        mDown = false;
        mCapture = nullptr;
        mShared.Count([](auto& stats) { ++stats.mPostedReleases; });
      }
      if (event.mKind == Kind::Leave && mHovered && IsWindow(mHovered)) {
        PostMessageW(mHovered, WM_MOUSELEAVE, 0, 0);
        mHovered = nullptr;
      }
      PostMessageW(top, mShared.mCommandMessage, static_cast<WPARAM>(Protocol::Command::EndInjection), 0);
      if (event.mKind != Kind::Up && event.mKind != Kind::Leave) {
        mShared.Count([](auto& stats) { ++stats.mSkippedHung; });
        const auto now = TimeUtil::Now();
        if (now >= mNextHungLog) {
          mNextHungLog = now + 10s;
          Log::Warning("Input: the window isn't responding; taps on it are skipped until it recovers");
        }
      }
      return;
    }

    // The point, in the window's own units
    POINT client {};
    POINT screen {};
    HWND child = top;
    POINT local {};
    {
      const WindowDpi dpi {top};
      RECT area {};
      GetClientRect(top, &area);
      client = ClientPoint(event.mTarget, event.mU, event.mV, area.right, area.bottom);
      screen = client;
      ClientToScreen(top, &screen);
      // During a press, the window that got it, like real mouse capture
      child = (mDown && mCapture && IsWindow(mCapture)) ? mCapture : DeepestChild(top, client);
      if (event.mKind == Kind::Wheel) {
        child = DeepestChild(top, client);
      }
      local = screen;
      ScreenToClient(child, &local);
    }

    Command(Protocol::Command::BeginInjection, Protocol::PackPoint(screen.x, screen.y));
    switch (event.mKind) {
      case Kind::Move: {
        if (!mDown && mHovered && mHovered != child && IsWindow(mHovered)) {
          Send(mHovered, WM_MOUSELEAVE, 0, 0);
        }
        Send(child, WM_MOUSEMOVE, mDown ? MK_LBUTTON : 0, PackClient(local));
        mHovered = child;
        mLastPoint = local;
        mShared.Count([](auto& stats) { ++stats.mMoves; });
        if (!mDown) {
          mNextHoverAt = TimeUtil::Now()
            + std::chrono::duration_cast<TimeUtil::Duration>(std::chrono::duration<double>(1.0 / mShared.mOptions.mHoverPerSecond));
        }
        break;
      }
      case Kind::Down: {
        // Not activated: measured, making the window active on its own thread
        // takes the real foreground whenever Windows allows it (the app was
        // started by the foreground process, or nobody touched the keyboard
        // or mouse for a while). Keys need it (doc 06, M5); clicks don't.
        const DWORD now = GetTickCount();
        const bool doubleTap = IsDoubleTap(child, screen, now);
        if (doubleTap) {
          // At the first tap's point: the app checks the two are close
          local = mLastDownLocal;
        }
        // INJ-6: a move to the exact point first, with no button yet
        Send(child, WM_MOUSEMOVE, 0, PackClient(local));
        Command(Protocol::Command::SetMouseButtons, MK_LBUTTON);
        Send(child, doubleTap ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, MK_LBUTTON, PackClient(local));
        mDown = true;
        mCapture = child;
        mHovered = child;
        mLastPoint = local;
        mLastDownTick = doubleTap ? 0 : now;// a third tap starts over
        mLastDownWindow = child;
        mLastDownScreen = screen;
        mLastDownLocal = local;
        const double waited = TimeUtil::ToMilliseconds(TimeUtil::Now() - event.mQueued);
        mShared.Count([&](auto& stats) {
          ++stats.mPresses;
          stats.mDoubleClicks += doubleTap ? 1 : 0;
          stats.mLongestWaitMs = std::max(stats.mLongestWaitMs, waited);
        });
        break;
      }
      case Kind::Up:
        if (mDown) {
          LetGo(child, local);
        }
        break;
      case Kind::Wheel:
        Send(child, WM_MOUSEWHEEL, MAKEWPARAM(mDown ? MK_LBUTTON : 0, static_cast<WORD>(static_cast<SHORT>(event.mWheel))),
          Protocol::PackPoint(screen.x, screen.y));
        mShared.Count([](auto& stats) { ++stats.mWheelMessages; });
        break;
      case Kind::Leave:
        // INJ-5: let go, and clear hover and tooltips
        if (mDown) {
          LetGo(child, mLastPoint);
        }
        if (mHovered && IsWindow(mHovered)) {
          Send(mHovered, WM_MOUSELEAVE, 0, 0);
        }
        mHovered = nullptr;
        break;
    }
    Command(Protocol::Command::EndInjection, 0);
  }

  Shared& mShared;
  const HWND mWindow;
  std::thread mThread;
  HANDLE mWake {CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  std::mutex mMutex;
  std::deque<Event> mQueue;
  bool mStop {false};

  // The worker thread's own (read by the destructor after it joined)
  bool mDown {false};
  bool mBusy {false};// a message timed out: hung until it answers
  HWND mCapture {nullptr};
  HWND mHovered {nullptr};
  POINT mLastPoint {};
  TimeUtil::TimePoint mNextHoverAt {};
  DWORD mLastDownTick {0};
  HWND mLastDownWindow {nullptr};
  POINT mLastDownScreen {};
  POINT mLastDownLocal {};
  TimeUtil::TimePoint mNextTimeoutLog {};
  TimeUtil::TimePoint mNextHungLog {};
};

}// namespace

struct Injector::Impl {
  Shared mShared;
  std::map<HWND, std::unique_ptr<Worker>> mWorkers;

  // The hand thread's view of the pointer
  bool mOnPanel {false};
  uint64_t mPanel {0};
  Target mTarget {};
  bool mPressed {false};
  float mU {0};
  float mV {0};
  Stats mLogged {};
  TimeUtil::TimePoint mNextPrune {};

  Worker& WorkerFor(HWND window) {
    auto& worker = mWorkers[window];
    if (!worker) {
      worker = std::make_unique<Worker>(mShared, window);
    }
    return *worker;
  }

  void Queue(Kind kind, const Target& target, float u, float v, int32_t wheel = 0, bool drag = false) {
    WorkerFor(target.mWindow).Enqueue({kind, target, u, v, wheel, drag, {}});
  }

  // Workers of windows that have gone
  void Prune() {
    const auto now = TimeUtil::Now();
    if (now < mNextPrune) {
      return;
    }
    mNextPrune = now + 5s;
    for (auto it = mWorkers.begin(); it != mWorkers.end();) {
      if (!IsWindow(it->first) && !(mOnPanel && mTarget.mWindow == it->first)) {
        it = mWorkers.erase(it);
      } else {
        ++it;
      }
    }
  }
};

Injector::Injector(Options options) : mImpl(std::make_unique<Impl>()) {
  mImpl->mShared.mOptions = options;
}

Injector::~Injector() {
  mImpl->mWorkers.clear();// each posts any release still held
}

void Injector::Pointer(uint64_t panelID, const Target* target, const CursorUpdate& update) {
  auto& impl = *mImpl;
  impl.Prune();
  const bool usable = target && target->mWindow && update.mState != TouchState::NotNear;
  const bool same = usable && impl.mOnPanel && impl.mPanel == panelID && impl.mTarget.mWindow == target->mWindow;
  // Leaving the panel (or it went): let go and clear hover (INJ-5)
  if (impl.mOnPanel && !same) {
    if (impl.mPressed) {
      impl.Queue(Kind::Up, impl.mTarget, impl.mU, impl.mV);
      impl.mPressed = false;
    }
    impl.Queue(Kind::Leave, impl.mTarget, impl.mU, impl.mV);
    impl.mOnPanel = false;
  }
  if (!usable) {
    return;
  }
  impl.mOnPanel = true;
  impl.mPanel = panelID;
  impl.mTarget = *target;
  impl.mU = update.mU;
  impl.mV = update.mV;
  if (update.mState == TouchState::Touching) {
    if (!impl.mPressed) {
      impl.Queue(Kind::Down, *target, update.mU, update.mV);
      impl.mPressed = true;
    } else {
      impl.Queue(Kind::Move, *target, update.mU, update.mV, 0, true);
    }
  } else {
    if (impl.mPressed) {
      impl.Queue(Kind::Up, *target, update.mU, update.mV);
      impl.mPressed = false;
    } else {
      impl.Queue(Kind::Move, *target, update.mU, update.mV);
    }
  }
  if (update.mWheel != 0) {
    impl.Queue(Kind::Wheel, *target, update.mU, update.mV, update.mWheel);
  }
}

Stats Injector::GetStats() const {
  std::scoped_lock lock(mImpl->mShared.mStatsMutex);
  return mImpl->mShared.mStats;
}

void Injector::LogStats(const char* title) {
  const auto now = GetStats();
  auto& last = mImpl->mLogged;
  if (now.mPresses == last.mPresses && now.mMoves == last.mMoves && now.mWheelMessages == last.mWheelMessages
      && now.mTimeouts == last.mTimeouts && now.mSkippedHung == last.mSkippedHung) {
    return;
  }
  Log::Info(
    "{}: {} presses ({} double), {} releases, {} moves ({} merged), {} wheel messages; {} timed out, {} skipped (not "
    "responding), {} releases posted; longest wait for a press so far {:.1f} ms",
    title,
    now.mPresses - last.mPresses,
    now.mDoubleClicks - last.mDoubleClicks,
    now.mReleases - last.mReleases,
    now.mMoves - last.mMoves,
    now.mMovesMerged - last.mMovesMerged,
    now.mWheelMessages - last.mWheelMessages,
    now.mTimeouts - last.mTimeouts,
    now.mSkippedHung - last.mSkippedHung,
    now.mPostedReleases - last.mPostedReleases,
    now.mLongestWaitMs);
  last = now;
}

}// namespace OpenDesktopVR::Pointer
