// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/AppHelperProtocol.hpp>
#include <OpenDesktopVR/InjectionHookProtocol.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
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

enum class Kind { Move, Down, Up, Wheel, Leave, Key };

struct Event {
  Kind mKind {Kind::Move};
  Target mTarget;
  float mU {0};
  float mV {0};
  int32_t mWheel {0};
  bool mDrag {false};// a move with the button held
  // Presses: this press's number, and the press it's the second tap of (0:
  // none). The worker sends a double-click only if it delivered that press.
  uint64_t mPress {0};
  uint64_t mDoubleOf {0};
  TimeUtil::TimePoint mQueued {};
  VirtualKeyStroke mStroke {};// Key
};

// The app that owns a window, for log lines; never sends it anything
std::string AppName(HWND window) {
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  const auto path = process ? ProcessUtil::ProcessImagePath(process) : std::filesystem::path {};
  return path.empty() ? std::string {"an app"} : StringUtil::Narrow(path.filename().wstring());
}

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
      ThreadUtil::CatchAll("input worker", [this] {
        mAppName = AppName(mWindow);
        Main();
      });
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
    const auto sentAt = TimeUtil::Now();
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(window, message, wParam, lParam, SMTO_NORMAL | SMTO_ABORTIFHUNG,
          static_cast<UINT>(mShared.mOptions.mTimeout.count()), &result)) {
      return true;
    }
    mBusy = true;
    StoppedResponding(sentAt);
    mShared.Count([](auto& stats) { ++stats.mTimeouts; });
    const auto now = TimeUtil::Now();
    if (now >= mNextTimeoutLog) {
      mNextTimeoutLog = now + 10s;
      Log::Warning("Input: {} took more than {} ms to take a message; carrying on without it", mAppName,
        mShared.mOptions.mTimeout.count());
    }
    return false;
  }

  void StoppedResponding(TimeUtil::TimePoint since) {
    if (!mNotRespondingSince) {
      mNotRespondingSince = since;
      mSkippedPresses = 0;
      mSkippedKeys = 0;
      mSkippedOther = 0;
    }
  }

  // A window that didn't take a message in time may still be working
  // through it; until it answers again it's treated as hung (INJ-3), so
  // clicks don't pile up in it to all land at once when it recovers
  bool Responding(HWND top) {
    bool responding = true;
    if (IsHungAppWindow(top)) {
      responding = false;
    } else if (mBusy) {
      DWORD_PTR result = 0;
      responding = SendMessageTimeoutW(top, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 50, &result) != 0;
      mBusy = !responding;
    }
    if (!responding) {
      StoppedResponding(TimeUtil::Now());
    } else if (mNotRespondingSince) {
      // Checked at the next event, so "within"
      Log::Info(
        "Input: {} is answering again, within {:.0f} ms of stopping; skipped meanwhile: {} taps, {} keys, {} moves or "
        "wheel",
        mAppName, TimeUtil::ToMilliseconds(TimeUtil::Now() - *mNotRespondingSince), mSkippedPresses, mSkippedKeys,
        mSkippedOther);
      mNotRespondingSince.reset();
    }
    return responding;
  }

  void Command(Protocol::Command command, LPARAM argument) {
    Send(mWindow, mShared.mCommandMessage, static_cast<WPARAM>(command), argument);
  }

  // The hand thread decided it's a second tap; it's sent as a double-click
  // if the first tap's press was delivered, to this window, and the window
  // wants double-clicks
  bool IsDoubleTap(const Event& event, HWND window) const {
    return event.mDoubleOf != 0 && event.mDoubleOf == mLastPress && mLastDownWindow == window
      && (GetClassLongPtrW(window, GCL_STYLE) & CS_DBLCLKS) != 0;
  }

  // The thread's focus window, if it's in `top`. With none, the control the
  // last press went to: the app may have lost its focus when the game got
  // the foreground back (R1, ADR-035), and that's where the user tapped.
  HWND KeyTarget(HWND top) const {
    const HWND focus = WindowKeyInjection::GetKeyTarget(top);
    if (focus == top && mLastDownWindow && mLastDownWindow != top && IsWindow(mLastDownWindow)
        && GetAncestor(mLastDownWindow, GA_ROOT) == top) {
      return mLastDownWindow;
    }
    return focus;
  }

  // A key press (doc 06 "Keyboard injection")
  void DeliverKey(HWND top, const VirtualKeyStroke& stroke) {
    const auto messages = WindowKeyInjection::GetMessages(stroke, mShared.mCommandMessage);
    if (messages.empty()) {
      // INJ-8: Alt+F4, Alt+Tab, a lone Alt, the Windows key...
      mShared.Count([](auto& stats) { ++stats.mKeysBlocked; });
      const auto now = TimeUtil::Now();
      if (now >= mNextBlockedLog) {
        mNextBlockedLog = now + 10s;
        Log::Info("Input: a key combination that would close or leave {} wasn't sent", mAppName);
      }
      return;
    }
    POINT cursor {};
    if (mLastScreen) {
      cursor = *mLastScreen;
    } else {
      RECT bounds {};
      GetWindowRect(top, &bounds);
      cursor = {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    }
    Command(Protocol::Command::BeginInjection, Protocol::PackPoint(cursor.x, cursor.y));
    // Firefox ignores keys in a window it doesn't think is active; the
    // helper does it on the app's own thread, only if it isn't already
    Send(mWindow, mShared.mCommandMessage, static_cast<WPARAM>(InjectionHookProtocol::Command::ActivateWindow),
      reinterpret_cast<LPARAM>(mWindow));
    // INJ-7: where keys go is decided now, after the taps before it landed
    // and the activation (an app may move its focus then)
    const HWND keyTarget = KeyTarget(top);
    Send(mWindow, mShared.mCommandMessage, static_cast<WPARAM>(InjectionHookProtocol::Command::SetKeyboardModifiers),
      WindowKeyInjection::GetModifierBits(stroke));
    for (const auto& message: messages) {
      Send(keyTarget, message.mMessage, message.mWParam, message.mLParam);
    }
    Command(Protocol::Command::EndInjection, 0);
    mShared.Count([](auto& stats) { ++stats.mKeys; });
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
        const bool press = event.mKind == Kind::Down;
        const bool key = event.mKind == Kind::Key;
        (press ? mSkippedPresses : key ? mSkippedKeys : mSkippedOther) += 1;
        mShared.Count([press, key](auto& stats) {
          ++stats.mSkippedHung;
          stats.mSkippedPresses += press ? 1 : 0;
          stats.mKeysSkipped += key ? 1 : 0;
        });
        const auto now = TimeUtil::Now();
        if (now >= mNextHungLog) {
          mNextHungLog = now + 10s;
          Log::Warning("Input: {} isn't responding; taps on it are skipped until it answers again", mAppName);
        }
      }
      return;
    }
    if (event.mKind == Kind::Key) {
      DeliverKey(top, event.mStroke);
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
      mLastScreen = screen;
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
        const bool doubleTap = IsDoubleTap(event, child);
        if (doubleTap) {
          // At the first tap's point: the app checks the two are close
          local = mLastDownLocal;
        }
        // INJ-6: a move to the exact point first, with no button yet
        Send(child, WM_MOUSEMOVE, 0, PackClient(local));
        Command(Protocol::Command::SetMouseButtons, MK_LBUTTON);
        Send(child, doubleTap ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, MK_LBUTTON, PackClient(local));
        // Then made active on its own thread, if it isn't (doc 07 "Target
        // selection"; ADR-036): Chromium apps and Firefox only show the
        // caret of the field the press focused in an active window, and
        // the keyboard shows from that caret (VK-1). After the press, so the
        // press's latency never includes it (doc 11's gate: one activation
        // that took the foreground and gave it back cost a frame). Where
        // Windows lets the app take the real foreground, the helper gives it
        // straight back (ADR-035).
        Send(mWindow, mShared.mCommandMessage, static_cast<WPARAM>(InjectionHookProtocol::Command::ActivateWindow),
          reinterpret_cast<LPARAM>(mWindow));
        mDown = true;
        mCapture = child;
        mHovered = child;
        mLastPoint = local;
        mLastPress = event.mPress;
        mLastDownWindow = child;
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
  uint64_t mLastPress {0};// the last press delivered
  HWND mLastDownWindow {nullptr};
  POINT mLastDownLocal {};
  std::string mAppName {"an app"};
  std::optional<TimeUtil::TimePoint> mNotRespondingSince;
  uint64_t mSkippedPresses {0};// while not responding, for its log line
  uint64_t mSkippedKeys {0};
  uint64_t mSkippedOther {0};
  // The last pointer event's screen point: where the cursor is for a key
  std::optional<POINT> mLastScreen;
  TimeUtil::TimePoint mNextTimeoutLog {};
  TimeUtil::TimePoint mNextHungLog {};
  TimeUtil::TimePoint mNextBlockedLog {};
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

  // Double taps, from the finger's own timing (ADR-034, amended)
  uint64_t mNextPress {1};
  uint64_t mPress {0};// the press held now, or the last one
  TimeUtil::TimePoint mPressedAt {};
  float mPressU {0};
  float mPressV {0};
  bool mPressIsSecond {false};
  // The last press, if it was a tap: lifted within the double-click time,
  // where it went down
  struct Tap {
    uint64_t mPress {0};
    uint64_t mPanel {0};
    TimeUtil::TimePoint mLiftedAt {};
    float mU {0};
    float mV {0};
  };
  std::optional<Tap> mLastTap;

  // INJ-7: keys may come from another thread (the keyboard's repeat), so
  // workers are made, fed and pruned under this lock. Feeding never waits.
  std::mutex mWorkersMutex;

  void Enqueue(HWND window, Event event) {
    std::scoped_lock lock(mWorkersMutex);
    auto& worker = mWorkers[window];
    if (!worker) {
      worker = std::make_unique<Worker>(mShared, window);
    }
    worker->Enqueue(std::move(event));
  }

  void Queue(Kind kind, const Target& target, float u, float v, int32_t wheel = 0, bool drag = false) {
    Enqueue(target.mWindow, {kind, target, u, v, wheel, drag, 0, 0, {}});
  }

  // Two points on the panel close enough for a double tap: within the
  // distance each way, on the panel in VR (or in its pixels, if its size
  // isn't known)
  bool Near(const Target& target, float u1, float v1, float u2, float v2) const {
    const auto& options = mShared.mOptions;
    if (target.mWidthMeters > 0 && target.mHeightMeters > 0) {
      const float limit = options.mDoubleTapMillimeters / 1000.0f;
      return std::abs(u2 - u1) * target.mWidthMeters <= limit && std::abs(v2 - v1) * target.mHeightMeters <= limit;
    }
    const auto width = static_cast<float>(target.mShown.right - target.mShown.left);
    const auto height = static_cast<float>(target.mShown.bottom - target.mShown.top);
    const auto limit = static_cast<float>(options.mDoubleTapPixels);
    return std::abs(u2 - u1) * width <= limit && std::abs(v2 - v1) * height <= limit;
  }

  void Press(uint64_t panelID, const Target& target, float u, float v) {
    const auto now = TimeUtil::Now();
    const auto window = std::chrono::milliseconds(GetDoubleClickTime());
    uint64_t doubleOf = 0;
    if (mLastTap && mLastTap->mPanel == panelID && now - mLastTap->mLiftedAt <= window
        && Near(target, mLastTap->mU, mLastTap->mV, u, v)) {
      doubleOf = mLastTap->mPress;
    }
    mLastTap.reset();// a third tap starts over
    mPress = mNextPress++;
    mPressedAt = now;
    mPressU = u;
    mPressV = v;
    mPressIsSecond = doubleOf != 0;
    Enqueue(target.mWindow, {Kind::Down, target, u, v, 0, false, mPress, doubleOf, {}});
  }

  void Lift(uint64_t panelID, const Target& target, float u, float v) {
    const auto now = TimeUtil::Now();
    const auto window = std::chrono::milliseconds(GetDoubleClickTime());
    mLastTap.reset();
    if (!mPressIsSecond && now - mPressedAt <= window && Near(target, mPressU, mPressV, u, v)) {
      mLastTap = Tap {mPress, panelID, now, mPressU, mPressV};
    }
    Queue(Kind::Up, target, u, v);
  }

  // Workers of windows that have gone
  void Prune() {
    const auto now = TimeUtil::Now();
    if (now < mNextPrune) {
      return;
    }
    mNextPrune = now + 5s;
    std::scoped_lock lock(mWorkersMutex);
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
    impl.mLastTap.reset();
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
      impl.Press(panelID, *target, update.mU, update.mV);
      impl.mPressed = true;
    } else {
      impl.Queue(Kind::Move, *target, update.mU, update.mV, 0, true);
    }
  } else {
    if (impl.mPressed) {
      impl.Lift(panelID, *target, update.mU, update.mV);
      impl.mPressed = false;
    } else {
      impl.Queue(Kind::Move, *target, update.mU, update.mV);
    }
  }
  if (update.mWheel != 0) {
    impl.Queue(Kind::Wheel, *target, update.mU, update.mV, update.mWheel);
  }
}

void Injector::Key(const Target& target, const VirtualKeyStroke& stroke) {
  if (!target.mWindow) {
    return;
  }
  Event event {Kind::Key, target};
  event.mStroke = stroke;
  mImpl->Enqueue(target.mWindow, std::move(event));
}

Stats Injector::GetStats() const {
  std::scoped_lock lock(mImpl->mShared.mStatsMutex);
  return mImpl->mShared.mStats;
}

void Injector::LogStats(const char* title) {
  const auto now = GetStats();
  auto& last = mImpl->mLogged;
  if (now.mPresses == last.mPresses && now.mMoves == last.mMoves && now.mWheelMessages == last.mWheelMessages
      && now.mTimeouts == last.mTimeouts && now.mSkippedHung == last.mSkippedHung && now.mKeys == last.mKeys
      && now.mKeysBlocked == last.mKeysBlocked) {
    return;
  }
  Log::Info(
    "{}: {} presses ({} double), {} releases, {} moves ({} merged), {} wheel messages, {} keys ({} blocked); {} timed "
    "out, {} skipped (not responding; {} of them taps, {} keys), {} releases posted; longest wait for a press so far "
    "{:.1f} ms",
    title,
    now.mPresses - last.mPresses,
    now.mDoubleClicks - last.mDoubleClicks,
    now.mReleases - last.mReleases,
    now.mMoves - last.mMoves,
    now.mMovesMerged - last.mMovesMerged,
    now.mWheelMessages - last.mWheelMessages,
    now.mKeys - last.mKeys,
    now.mKeysBlocked - last.mKeysBlocked,
    now.mTimeouts - last.mTimeouts,
    now.mSkippedHung - last.mSkippedHung,
    now.mSkippedPresses - last.mSkippedPresses,
    now.mKeysSkipped - last.mKeysSkipped,
    now.mPostedReleases - last.mPostedReleases,
    now.mLongestWaitMs);
  last = now;
}

}// namespace OpenDesktopVR::Pointer
