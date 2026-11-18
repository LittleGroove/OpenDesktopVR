// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/AppHelperProtocol.hpp>
#include <OpenDesktopVR/AppHelperRules.hpp>
#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/CrashHandler.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace OpenDesktopVR::AppHelper {

namespace Protocol = AppHelperProtocol;
using namespace std::chrono_literals;

namespace {

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.AppHelper";
constexpr UINT WakeMessage = WM_APP + 1;
constexpr UINT StopMessage = WM_APP + 2;
constexpr UINT_PTR DeadlineTimer = 1;
constexpr UINT DeadlineCheckMs = 250;
// On exit: how long the apps get to unload the helper
constexpr auto ShutdownWait = 1500ms;

// Another process's window: GetWindowText reads the stored title, and never
// sends it a message
std::string TitleOf(HWND window) {
  wchar_t text[256] {};
  GetWindowTextW(window, text, static_cast<int>(std::size(text)));
  return StringUtil::Narrow(text);
}

std::string ExeOf(DWORD process) {
  const auto path = ProcessUtil::ProcessImagePath(process);
  return path.empty() ? std::string {"the app"} : StringUtil::Narrow(path.filename().wstring());
}

}// namespace

struct AppHelperService::Impl : std::enable_shared_from_this<Impl> {
  // What a lease is for
  enum class Kind : int { KeepPainting = 0, Input = 1 };
  static constexpr Protocol::Command OnCommand(Kind kind) {
    return kind == Kind::Input ? Protocol::Command::InputOn : Protocol::Command::KeepPaintingOn;
  }
  static constexpr Protocol::Command OffCommand(Kind kind) {
    return kind == Kind::Input ? Protocol::Command::InputOff : Protocol::Command::KeepPaintingOff;
  }
  using PanelKey = std::pair<uint64_t, Kind>;

  struct Request {
    bool mAttach {true};
    uint64_t mLease {0};
    uint64_t mPanel {0};
    HWND mWindow {nullptr};
    Kind mKind {Kind::KeepPainting};
  };
  struct Lease {
    uint64_t mID {0};
    uint64_t mPanel {0};
    HWND mWindow {nullptr};
    Kind mKind {Kind::KeepPainting};
    DWORD mThread {0};// its install's thread, once connected
    std::string mTitle;
    std::string mExe;
    bool mOnSent {false};
  };
  enum class Phase { Hello, Ready, Unloading };
  struct Install {
    DWORD mThread {0};
    HHOOK mHook {nullptr};
    HHOOK mCallHook {nullptr};// the injector's sent commands
    std::string mExe;
    Phase mPhase {Phase::Hello};
    TimeUtil::TimePoint mDeadline {};
    std::vector<uint64_t> mLeases;
    // In the order sent, per kind; the helper answers in order
    std::deque<uint64_t> mAwaitingOn[2];
    uint32_t mAwaitingOff[2] {0, 0};
  };

  explicit Impl(Options options) : mOptions(std::move(options)) {
  }

  Options mOptions;
  std::thread mThread;
  std::atomic<HWND> mWindow {nullptr};
  std::atomic<bool> mDone {false};

  // Shared with other threads
  mutable std::mutex mMutex;
  std::vector<Request> mRequests;
  std::map<PanelKey, uint64_t> mNewestLease;// (panel, kind) -> lease
  std::map<PanelKey, Status> mStatus;       // (panel, kind) -> status of its newest lease
  std::function<uint32_t()> mGameProcess;
  std::size_t mInstallCount {0};
  uint64_t mUnexpected {0};
  uint64_t mNextLease {1};

  // The service thread's own
  HMODULE mHelper {nullptr};
  HOOKPROC mHookProc {nullptr};
  HOOKPROC mCallHookProc {nullptr};
  UINT mCommandMessage {0};
  UINT mReplyMessage {0};
  std::map<uint64_t, Lease> mLeases;
  std::map<DWORD, Install> mInstalls;
  std::vector<uint64_t> mParked;// waiting for their thread's unload to finish
  bool mStopping {false};

  // ---- Any thread ----------------------------------------------------------

  void Queue(Request request) {
    {
      std::scoped_lock lock(mMutex);
      mRequests.push_back(request);
    }
    if (const HWND window = mWindow.load()) {
      PostMessageW(window, WakeMessage, 0, 0);
    }
  }

  void SetStatus(const Lease& lease, State state, std::string text) {
    std::scoped_lock lock(mMutex);
    const PanelKey key {lease.mPanel, lease.mKind};
    if (auto newest = mNewestLease.find(key); newest != mNewestLease.end() && newest->second == lease.mID) {
      mStatus[key] = {state, std::move(text)};
    }
  }

  // ---- The service thread ----------------------------------------------------

  void Main() {
    ThreadUtil::SetCurrentThreadName(L"App helper");
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass {sizeof(windowClass)};
    windowClass.lpfnWndProc = &CrashHandler::GuardedProcedure<&Impl::WindowProc>;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WindowClass;
    RegisterClassExW(&windowClass);// may exist already (a second service in tests)
    const HWND window = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!window) {
      Log::Error("App helper: no window ({}); captured browsers will stop updating when covered", GetLastError());
      return;
    }
    mCommandMessage = RegisterWindowMessageW(Protocol::CommandMessageName);
    mReplyMessage = RegisterWindowMessageW(Protocol::ReplyMessageName);
    // Apps at a lower integrity level may answer too
    ChangeWindowMessageFilterEx(window, mReplyMessage, MSGFLT_ALLOW, nullptr);
    LoadHelper();
    SetTimer(window, DeadlineTimer, DeadlineCheckMs, nullptr);
    mWindow = window;
    ProcessRequests();// any queued before the window existed
    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      DispatchMessageW(&message);
    }
    mWindow = nullptr;
    // Whatever didn't answer in time: the hooks go now (the helper's DllMain
    // puts the app's imports back when it's unloaded)
    for (const auto& [thread, install]: mInstalls) {
      Unhook(install);
    }
    mInstalls.clear();
    DestroyWindow(window);
    if (mHelper) {
      FreeLibrary(mHelper);
    }
  }

  void LoadHelper() {
    const auto path = mOptions.mHelperPath.empty()
      ? ProcessUtil::ExecutablePath().parent_path() / Product::AppHelperDllName
      : mOptions.mHelperPath;
    mHelper = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (mHelper) {
      mHookProc = reinterpret_cast<HOOKPROC>(reinterpret_cast<void*>(GetProcAddress(mHelper, Protocol::HookProcName)));
      mCallHookProc = reinterpret_cast<HOOKPROC>(reinterpret_cast<void*>(GetProcAddress(mHelper, Protocol::CallHookProcName)));
    }
    if (!mHookProc || !mCallHookProc) {
      mHookProc = nullptr;
      Log::Warning("App helper: '{}' can't be used ({}); captured browsers will stop updating when covered",
        StringUtil::Narrow(path.wstring()), GetLastError());
    }
  }

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
      SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && (message == WakeMessage || message == StopMessage || message == WM_TIMER
          || (message == self->mReplyMessage && message != 0))) {
      ThreadUtil::CatchAll("app helper service", [&] {
        if (message == WakeMessage) {
          self->ProcessRequests();
        } else if (message == StopMessage) {
          self->Shutdown();
        } else if (message == WM_TIMER) {
          self->CheckDeadlines();
        } else {
          self->OnReply(wParam, lParam);
        }
      });
      return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
  }

  void ProcessRequests() {
    std::vector<Request> requests;
    {
      std::scoped_lock lock(mMutex);
      requests.swap(mRequests);
    }
    for (const auto& request: requests) {
      if (request.mAttach) {
        Attach(request);
      } else {
        Release(request.mLease);
      }
    }
  }

  // Whether this window's app should and can have the helper; the lease is
  // kept either way, for its status and its release
  void Attach(const Request& request) {
    Lease lease {request.mLease, request.mPanel, request.mWindow, request.mKind};
    const bool input = lease.mKind == Kind::Input;
    if (mStopping) {
      return;
    }
    if (!IsWindow(lease.mWindow)) {
      SetStatus(lease, State::Failed, "The window closed.");
      return;
    }
    wchar_t className[256] {};
    GetClassNameW(lease.mWindow, className, static_cast<int>(std::size(className)));
    lease.mTitle = TitleOf(lease.mWindow);
    const auto family = FamilyOf(className);
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(lease.mWindow, &process);
    lease.mExe = ExeOf(process);
    uint32_t game = 0;
    {
      std::scoped_lock lock(mMutex);
      game = mGameProcess ? mGameProcess() : 0;
    }
    const auto keep = [&](State state, std::string text) {
      SetStatus(lease, state, std::move(text));
      mLeases.emplace(lease.mID, lease);
    };
    if (!input && family == AppFamily::None) {
      keep(State::NotNeeded, "Not needed: this app keeps updating when covered.");
      return;
    }
    if (process == GetCurrentProcessId() || thread == 0) {
      keep(State::NotNeeded, "Not needed for this window.");
      return;
    }
    // HK-11: never into the game
    if (process == game) {
      Log::Warning("App helper: '{}' belongs to the game ({}): no helper there", lease.mTitle, lease.mExe);
      keep(State::Unavailable, "Never for the game.");
      return;
    }
    // What the user loses without it
    const char* without = input ? "Taps may not reach it." : "It may stop updating when covered.";
    if (ProcessUtil::ProcessElevation(process) != ProcessUtil::Elevation::NotElevated
        && ProcessUtil::CurrentProcessElevation() != ProcessUtil::Elevation::Elevated) {
      Log::Info("App helper: {} runs as administrator and we don't; '{}': {}", lease.mExe, lease.mTitle, without);
      keep(State::Unavailable, std::format("Not available: {} runs as administrator. {}", lease.mExe, without));
      return;
    }
    if (ProcessUtil::SameArchitecture(process) != true) {
      Log::Info("App helper: {} is a 32-bit app; '{}': {}", lease.mExe, lease.mTitle, without);
      keep(State::Unavailable, std::format("Not available: {} is a 32-bit app. {}", lease.mExe, without));
      return;
    }
    if (!mHookProc) {
      keep(State::Failed, "The helper file is missing: install again.");
      return;
    }
    lease.mThread = thread;
    mLeases.emplace(lease.mID, lease);
    Connect(lease.mID);
  }

  // Into its thread's install (made if needed), then On
  void Connect(uint64_t id) {
    auto& lease = mLeases.at(id);
    auto it = mInstalls.find(lease.mThread);
    if (it != mInstalls.end() && it->second.mPhase == Phase::Unloading) {
      mParked.push_back(id);// a new install once this one is gone
      SetStatus(lease, State::Waiting, "Starting...");
      return;
    }
    if (it == mInstalls.end()) {
      // HK-2: one set of hooks per UI thread, for all its windows
      const HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, mHookProc, mHelper, lease.mThread);
      const HHOOK callHook = hook ? SetWindowsHookExW(WH_CALLWNDPROC, mCallHookProc, mHelper, lease.mThread) : nullptr;
      if (!hook || !callHook) {
        const auto error = GetLastError();
        if (hook) {
          UnhookWindowsHookEx(hook);
        }
        Log::Warning("App helper: can't hook {} ({}) for '{}'", lease.mExe, error, lease.mTitle);
        SetStatus(lease, State::Failed, std::format("The helper can't load into {} (error {}).", lease.mExe, error));
        lease.mThread = 0;
        return;
      }
      Install install;
      install.mThread = lease.mThread;
      install.mHook = hook;
      install.mCallHook = callHook;
      install.mExe = lease.mExe;
      install.mDeadline = TimeUtil::Now() + mOptions.mHelloTimeout;
      it = mInstalls.emplace(lease.mThread, std::move(install)).first;
      UpdateInstallCount();
      // HK-4: only now, with the hook in; the first command waits for its
      // answer before anything counts as on
      if (!Post(it->second, lease.mWindow, Protocol::Command::Hello, reinterpret_cast<LPARAM>(mWindow.load()))) {
        SetStatus(lease, State::Failed, "The window closed.");
        Remove(lease.mThread);
        lease.mThread = 0;
        return;
      }
    }
    auto& install = it->second;
    install.mLeases.push_back(id);
    if (Post(install, lease.mWindow, OnCommand(lease.mKind), reinterpret_cast<LPARAM>(lease.mWindow))) {
      lease.mOnSent = true;
      install.mAwaitingOn[static_cast<int>(lease.mKind)].push_back(id);
      SetStatus(lease, State::Waiting, std::format("Loading the helper into {}...", lease.mExe));
    }
  }

  // To the window if it's still there, else another of the install's
  // windows, else the thread itself. Posted: never waits on the app.
  bool Post(Install& install, HWND preferred, Protocol::Command command, LPARAM argument) {
    const auto wParam = static_cast<WPARAM>(command);
    if (preferred && IsWindow(preferred) && PostMessageW(preferred, mCommandMessage, wParam, argument)) {
      return true;
    }
    for (const auto id: install.mLeases) {
      const HWND window = mLeases.at(id).mWindow;
      if (window != preferred && IsWindow(window) && PostMessageW(window, mCommandMessage, wParam, argument)) {
        return true;
      }
    }
    return PostThreadMessageW(install.mThread, mCommandMessage, wParam, argument) != FALSE;
  }

  void Release(uint64_t id) {
    std::erase(mParked, id);
    const auto found = mLeases.find(id);
    if (found == mLeases.end()) {
      return;
    }
    const Lease lease = found->second;
    mLeases.erase(found);
    {
      std::scoped_lock lock(mMutex);
      const PanelKey key {lease.mPanel, lease.mKind};
      if (auto newest = mNewestLease.find(key); newest != mNewestLease.end() && newest->second == id) {
        mNewestLease.erase(newest);
        mStatus.erase(key);
      }
    }
    const auto it = mInstalls.find(lease.mThread);
    if (mStopping || lease.mThread == 0 || it == mInstalls.end()) {
      return;
    }
    auto& install = it->second;
    std::erase(install.mLeases, id);
    // Another panel may still show the same window, for the same thing
    const bool stillShown = std::ranges::any_of(install.mLeases, [&](uint64_t other) {
      const auto& lease2 = mLeases.at(other);
      return lease2.mWindow == lease.mWindow && lease2.mKind == lease.mKind;
    });
    if (lease.mOnSent && !stillShown && install.mPhase != Phase::Unloading
        && Post(install, lease.mWindow, OffCommand(lease.mKind), reinterpret_cast<LPARAM>(lease.mWindow))) {
      ++install.mAwaitingOff[static_cast<int>(lease.mKind)];
    }
    if (install.mLeases.empty() && install.mPhase != Phase::Unloading) {
      StartUnload(install);
    }
  }

  void StartUnload(Install& install) {
    install.mPhase = Phase::Unloading;
    install.mDeadline = TimeUtil::Now() + (mStopping ? ShutdownWait : mOptions.mUnloadTimeout);
    if (!Post(install, nullptr, Protocol::Command::Unload, 0)) {
      Remove(install.mThread);// the thread is gone, and its hook with it
    }
  }

  // The hook goes, which unloads the helper from the app
  void Remove(DWORD thread) {
    const auto it = mInstalls.find(thread);
    if (it == mInstalls.end()) {
      return;
    }
    Unhook(it->second);
    mInstalls.erase(it);
    UpdateInstallCount();
    // Leases that arrived while it unloaded get a new install
    std::vector<uint64_t> parked;
    std::erase_if(mParked, [&](uint64_t id) {
      const bool mine = mLeases.contains(id) && mLeases.at(id).mThread == thread;
      if (mine) {
        parked.push_back(id);
      }
      return mine;
    });
    for (const auto id: parked) {
      if (!mStopping) {
        Connect(id);
      }
    }
    if (mStopping && mInstalls.empty()) {
      PostQuitMessage(0);
    }
  }

  // Windows unloads a hook's DLL from the app when its thread next takes a
  // message: one that does nothing, so an idle app lets go of it now (R8)
  static void Unhook(const Install& install) {
    UnhookWindowsHookEx(install.mHook);
    if (install.mCallHook) {
      UnhookWindowsHookEx(install.mCallHook);
    }
    PostThreadMessageW(install.mThread, WM_NULL, 0, 0);
  }

  void OnReply(WPARAM wParam, LPARAM lParam) {
    const auto thread = static_cast<DWORD>(lParam);
    const auto reply = Protocol::Unpack(wParam);
    const auto it = mInstalls.find(thread);
    if (it == mInstalls.end()) {
      Unexpected();
      return;
    }
    auto& install = it->second;
    switch (reply.mCommand) {
      case Protocol::Command::Hello:
        if (install.mPhase != Phase::Hello) {
          Unexpected();
        } else if (reply.mResult != Protocol::Result::Ok || reply.mDetail != Protocol::Version) {
          // An older helper, still loaded (it couldn't unload before)
          Log::Warning("App helper: {} has a helper from another build loaded; restart {} to update it", install.mExe,
            install.mExe);
          FailAll(install, std::format("Restart {}: it has an older helper loaded.", install.mExe));
          Remove(thread);
        } else {
          install.mPhase = Phase::Ready;
        }
        break;
      case Protocol::Command::KeepPaintingOn:
      case Protocol::Command::InputOn: {
        auto& awaiting = install.mAwaitingOn[static_cast<int>(KindOf(reply.mCommand))];
        if (awaiting.empty()) {
          Unexpected();
        } else {
          const auto id = awaiting.front();
          awaiting.pop_front();
          if (const auto lease = mLeases.find(id); lease != mLeases.end()) {
            Answered(lease->second, reply);
          }
        }
        break;
      }
      case Protocol::Command::KeepPaintingOff:
      case Protocol::Command::InputOff: {
        auto& awaiting = install.mAwaitingOff[static_cast<int>(KindOf(reply.mCommand))];
        if (awaiting == 0) {
          Unexpected();
        } else {
          --awaiting;
        }
        break;
      }
      case Protocol::Command::Unload:
        if (install.mPhase != Phase::Unloading) {
          Unexpected();
          break;
        }
        if (reply.mResult == Protocol::Result::StillBusy) {
          Log::Warning("App helper: {} was still in the helper after the wait; it stays loaded, switched off, until it closes",
            install.mExe);
        }
        Remove(thread);
        break;
      default:
        Unexpected();
        break;
    }
  }

  static Kind KindOf(Protocol::Command command) {
    return command == Protocol::Command::InputOn || command == Protocol::Command::InputOff ? Kind::Input : Kind::KeepPainting;
  }

  void Answered(const Lease& lease, const Protocol::Reply& reply) {
    if (lease.mKind == Kind::Input) {
      // Without anything to redirect, taps still go to the app as messages
      if (reply.mResult == Protocol::Result::Ok || reply.mResult == Protocol::Result::NothingToPatch) {
        Log::Info("App helper: '{}' ({}) is ready for taps ({} import{} redirected)", lease.mTitle, lease.mExe,
          reply.mDetail, reply.mDetail == 1 ? "" : "s");
        SetStatus(lease, State::On, std::format("On: the helper is loaded in {}.", lease.mExe));
      } else {
        Log::Warning("App helper: couldn't start input in {} (result {})", lease.mExe, static_cast<int>(reply.mResult));
        SetStatus(lease, State::Failed, std::format("The helper couldn't start in {}. Taps may not reach it.", lease.mExe));
      }
      return;
    }
    switch (reply.mResult) {
      case Protocol::Result::Ok:
        Log::Info("App helper: '{}' ({}) keeps updating when covered ({} import{} redirected)", lease.mTitle, lease.mExe,
          reply.mDetail, reply.mDetail == 1 ? "" : "s");
        SetStatus(lease, State::On, std::format("On: the helper is loaded in {}.", lease.mExe));
        break;
      case Protocol::Result::NothingToPatch:
        Log::Warning("App helper: {} doesn't import the visibility check; '{}' may stop updating when covered", lease.mExe,
          lease.mTitle);
        SetStatus(lease, State::Failed,
          std::format("{} doesn't use the check the helper knows. It may stop updating when covered.", lease.mExe));
        break;
      default:
        Log::Warning("App helper: couldn't start in {} (result {})", lease.mExe, static_cast<int>(reply.mResult));
        SetStatus(lease, State::Failed, std::format("The helper couldn't start in {}.", lease.mExe));
        break;
    }
  }

  void FailAll(Install& install, const std::string& text) {
    for (const auto id: install.mLeases) {
      auto& lease = mLeases.at(id);
      SetStatus(lease, State::Failed, text);
      lease.mThread = 0;
    }
    install.mLeases.clear();
  }

  void CheckDeadlines() {
    const auto now = TimeUtil::Now();
    std::vector<DWORD> late;
    for (const auto& [thread, install]: mInstalls) {
      if (install.mPhase != Phase::Ready && now >= install.mDeadline) {
        late.push_back(thread);
      }
    }
    for (const auto thread: late) {
      auto& install = mInstalls.at(thread);
      if (install.mPhase == Phase::Hello) {
        Log::Warning("App helper: no answer from {} in {} s (it may not allow helpers, or it's busy)", install.mExe,
          std::chrono::duration_cast<std::chrono::seconds>(mOptions.mHelloTimeout).count());
        FailAll(install, std::format("No answer from {}. It may stop updating when covered.", install.mExe));
      } else {
        Log::Warning("App helper: {} didn't confirm unloading; removing the hook anyway", install.mExe);
      }
      Remove(thread);
    }
  }

  void Shutdown() {
    mStopping = true;
    mParked.clear();
    std::vector<DWORD> threads;
    for (const auto& [thread, install]: mInstalls) {
      threads.push_back(thread);
    }
    for (const auto thread: threads) {
      if (const auto it = mInstalls.find(thread); it != mInstalls.end()) {
        if (it->second.mPhase == Phase::Unloading) {
          it->second.mDeadline = std::min(it->second.mDeadline, TimeUtil::Now() + ShutdownWait);
        } else {
          StartUnload(it->second);
        }
      }
    }
    if (mInstalls.empty()) {
      PostQuitMessage(0);
    }
  }

  void Unexpected() {
    std::scoped_lock lock(mMutex);
    ++mUnexpected;
  }

  void UpdateInstallCount() {
    std::scoped_lock lock(mMutex);
    mInstallCount = mInstalls.size();
  }
};

AppHelperService::AppHelperService() : AppHelperService(Options {}) {
}

AppHelperService::AppHelperService(Options options) : mImpl(std::make_shared<Impl>(std::move(options))) {
  mImpl->mThread = std::thread([impl = mImpl.get()] {
    ThreadUtil::CatchAll("app helper service", [&] { impl->Main(); });
    impl->mDone = true;
  });
}

AppHelperService::~AppHelperService() {
  // The thread posts its own quit once every app has unloaded the helper,
  // or the wait ran out
  while (!mImpl->mDone) {
    if (const HWND window = mImpl->mWindow.load()) {
      PostMessageW(window, StopMessage, 0, 0);
      break;
    }
    Sleep(5);// still starting
  }
  if (mImpl->mThread.joinable()) {
    mImpl->mThread.join();
  }
}

void AppHelperService::SetGameProcess(std::function<uint32_t()> gameProcess) {
  std::scoped_lock lock(mImpl->mMutex);
  mImpl->mGameProcess = std::move(gameProcess);
}

std::shared_ptr<void> AppHelperService::KeepPainting(uint64_t panelID, HWND window) {
  return MakeLease(panelID, window, static_cast<int>(Impl::Kind::KeepPainting));
}

std::shared_ptr<void> AppHelperService::Input(uint64_t panelID, HWND window) {
  return MakeLease(panelID, window, static_cast<int>(Impl::Kind::Input));
}

std::shared_ptr<void> AppHelperService::MakeLease(uint64_t panelID, HWND window, int kindValue) {
  const auto kind = static_cast<Impl::Kind>(kindValue);
  uint64_t id = 0;
  {
    std::scoped_lock lock(mImpl->mMutex);
    id = mImpl->mNextLease++;
    mImpl->mNewestLease[{panelID, kind}] = id;
    mImpl->mStatus[{panelID, kind}] = {State::Waiting, "Starting..."};
  }
  mImpl->Queue({true, id, panelID, window, kind});
  // Released from any thread, even after the service is gone. Made in place:
  // a copy's destructor would release it at once.
  struct Token {
    std::weak_ptr<Impl> mImpl;
    uint64_t mID {0};
    ~Token() {
      if (auto impl = mImpl.lock()) {
        impl->Queue({false, mID, 0, nullptr, Impl::Kind::KeepPainting});
      }
    }
  };
  auto token = std::make_shared<Token>();
  token->mImpl = mImpl;
  token->mID = id;
  return token;
}

std::optional<AppHelperService::Status> AppHelperService::StatusOf(uint64_t panelID) const {
  std::scoped_lock lock(mImpl->mMutex);
  if (const auto it = mImpl->mStatus.find({panelID, Impl::Kind::KeepPainting}); it != mImpl->mStatus.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<AppHelperService::Status> AppHelperService::InputStatusOf(uint64_t panelID) const {
  std::scoped_lock lock(mImpl->mMutex);
  if (const auto it = mImpl->mStatus.find({panelID, Impl::Kind::Input}); it != mImpl->mStatus.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::size_t AppHelperService::Installs() const {
  std::scoped_lock lock(mImpl->mMutex);
  return mImpl->mInstallCount;
}

uint64_t AppHelperService::UnexpectedReplies() const {
  std::scoped_lock lock(mImpl->mMutex);
  return mImpl->mUnexpected;
}

}// namespace OpenDesktopVR::AppHelper
