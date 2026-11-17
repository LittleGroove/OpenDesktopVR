// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The injector (M4b, doc 06) against a real Win32 window in another process
// (click-target, which logs what it gets), with the app helper's input mode
// loaded into it, while this test's own window is the foreground (the game):
// - a tap: one press and one release where the finger was, after a move to
//   the exact point; the foreground and the click target's place in the
//   z-order don't change (R1);
// - hover: at most about 30 moves a second (doc 05);
// - a double tap becomes a double-click: timed from the first tap's lift,
//   within 15 mm on the panel; not after a long hold, not 20 mm apart, not
//   after the double-click time, and a third tap starts over;
// - the wheel, with its point;
// - leaving the panel lets go and clears hover (INJ-5); the panel going
//   away mid-drag lets go (INJ-4); the injector going away mid-drag posts
//   the release (INJ-4);
// - a slow app (20 ms per move): moves merge, a press waits for no backlog
//   (INJ-1);
// - a hung app: the release posted, later taps skipped, no stuck button and
//   no burst of late clicks when it recovers (INJ-3, R4);
// - where a point on the panel lands, through the part of the window shown.
//
//   pointer-inject-test <click-target exe>

#include "check.hpp"
#include "foreground.hpp"

#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace std::chrono_literals;
using HandInteraction::CursorUpdate;
using HandInteraction::TouchState;
using Testing::Check;
using Testing::Scenario;

namespace {

std::filesystem::path gClickTarget;
std::filesystem::path gLogFile;

double NowMilliseconds() {
  LARGE_INTEGER frequency {};
  LARGE_INTEGER now {};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

void Pump(std::chrono::milliseconds duration) {
  const auto end = std::chrono::steady_clock::now() + duration;
  do {
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(2ms);
  } while (std::chrono::steady_clock::now() < end);
}

bool WaitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    Pump(10ms);
  }
  return condition();
}

// A click-target in its own process, and what it logged
struct Target {
  PROCESS_INFORMATION mProcess {};
  std::filesystem::path mLog;
  HWND mWindow {nullptr};

  explicit Target(const std::wstring& extra = {}) {
    mLog = std::filesystem::temp_directory_path() / std::format(L"odvr-pointer-{}-{}.log", GetCurrentProcessId(), ++Counter());
    std::error_code ignored;
    std::filesystem::remove(mLog, ignored);
    auto command = std::format(L"\"{}\" \"{}\" {}", gClickTarget.wstring(), mLog.wstring(), extra);
    STARTUPINFOW startup {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNOACTIVATE;
    if (!CreateProcessW(gClickTarget.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
          &startup, &mProcess)) {
      return;
    }
    WaitFor([&] { return Has("READY"); }, 5s);
    WaitFor([&] {
      mWindow = FindWindowW(L"OpenDesktopVR.ClickTarget", nullptr);
      DWORD process = 0;
      GetWindowThreadProcessId(mWindow, &process);
      return mWindow && process == mProcess.dwProcessId;
    }, 5s);
  }
  ~Target() {
    if (mProcess.hProcess) {
      TerminateProcess(mProcess.hProcess, 0);
      WaitForSingleObject(mProcess.hProcess, 5000);
      CloseHandle(mProcess.hProcess);
      CloseHandle(mProcess.hThread);
    }
    std::error_code ignored;
    std::filesystem::remove(mLog, ignored);
  }
  static int& Counter() {
    static int counter = 0;
    return counter;
  }
  std::vector<std::string> Lines() const {
    std::vector<std::string> lines;
    std::ifstream file(mLog);
    for (std::string line; std::getline(file, line);) {
      lines.push_back(line);
    }
    return lines;
  }
  bool Has(std::string_view prefix) const {
    for (const auto& line: Lines()) {
      if (line.starts_with(prefix)) {
        return true;
      }
    }
    return false;
  }
  int Count(std::string_view prefix) const {
    int count = 0;
    for (const auto& line: Lines()) {
      count += line.starts_with(prefix) ? 1 : 0;
    }
    return count;
  }
  // The fields after a prefix, of its nth line (0: the first)
  std::vector<double> Fields(std::string_view prefix, int nth = 0) const {
    for (const auto& line: Lines()) {
      if (line.starts_with(prefix) && nth-- == 0) {
        std::istringstream in(line.substr(prefix.size()));
        std::vector<double> fields;
        for (double value; in >> value;) {
          fields.push_back(value);
        }
        return fields;
      }
    }
    return {};
  }
  // The last press or release line
  std::string LastButton() const {
    std::string last;
    for (const auto& line: Lines()) {
      if (line.starts_with("DOWN") || line.starts_with("UP") || line.starts_with("DOUBLE")) {
        last = line;
      }
    }
    return last;
  }
  Pointer::Target AsTarget() const {
    return {mWindow, {0, 0, 800, 600}, {800, 600}};
  }
  // Its client size in its own units (it may be DPI-unaware)
  SIZE Client() const {
    RECT area {};
    GetClientRect(mWindow, &area);
    return {area.right, area.bottom};
  }
};

CursorUpdate At(float u, float v, TouchState state, int32_t wheel = 0) {
  return {.mPanelID = 1, .mU = u, .mV = v, .mState = state, .mWheel = wheel};
}

// A tap on the panel: hover, press, release, at 90 updates a second; held
// for `held` (a quick mouse-like tap by default)
void Tap(Pointer::Injector& injector, const Pointer::Target& target, float u, float v,
  std::chrono::milliseconds held = 33ms) {
  for (int i = 0; i < 3; ++i) {
    injector.Pointer(1, &target, At(u, v, TouchState::Hovering));
    std::this_thread::sleep_for(11ms);
  }
  const auto until = std::chrono::steady_clock::now() + held;
  do {
    injector.Pointer(1, &target, At(u, v, TouchState::Touching));
    std::this_thread::sleep_for(11ms);
  } while (std::chrono::steady_clock::now() < until);
  injector.Pointer(1, &target, At(u, v, TouchState::Hovering));
}

// The double-click time, as the hand thread uses it
std::chrono::milliseconds DoubleClickTime() {
  return std::chrono::milliseconds(GetDoubleClickTime());
}

// This test's own window: the foreground, like the game
HWND MakeGameWindow() {
  WNDCLASSW windowClass {};
  windowClass.lpfnWndProc = &DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
  windowClass.lpszClassName = L"OdvrPointerTestGame";
  RegisterClassW(&windowClass);
  return CreateWindowExW(0, windowClass.lpszClassName, L"pointer-inject-test (the game)", WS_OVERLAPPED | WS_VISIBLE, 300, 300,
    400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

}// namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("Usage: pointer-inject-test <click-target exe>\n");
    return 2;
  }
  gClickTarget = std::filesystem::absolute(argv[1]);
  // The injector's log lines, checked by the hung-app scenario
  gLogFile = std::filesystem::temp_directory_path() / std::format(L"odvr-pointer-{}.log", GetCurrentProcessId());
  {
    std::error_code ignored;
    std::filesystem::remove(gLogFile, ignored);
    Log::Config config;
    config.mFile = gLogFile;
    config.mDebuggerOutput = false;
    Log::Start(config);
  }
  const HWND game = MakeGameWindow();
  AppHelper::AppHelperService helper;

  Scenario("Where a point on the panel lands, through the part of the window shown", [] {
    const Pointer::Target whole {nullptr, {0, 0, 800, 600}, {800, 600}};
    const auto middle = Pointer::ClientPoint(whole, 0.5f, 0.5f, 800, 600);
    Check(middle.x == 400 && middle.y == 300, "the middle of the panel: the middle of the window");
    const auto scaled = Pointer::ClientPoint(whole, 0.5f, 0.5f, 400, 300);
    Check(scaled.x == 200 && scaled.y == 150, "a window in half the units (DPI-unaware, at 200%): half the point");
    const Pointer::Target trimmed {nullptr, {100, 50, 700, 550}, {800, 600}};
    const auto corner = Pointer::ClientPoint(trimmed, 0, 0, 800, 600);
    Check(corner.x == 100 && corner.y == 50, "trimmed: the panel's corner is the trim's corner");
    const auto edge = Pointer::ClientPoint(trimmed, 1, 1, 800, 600);
    Check(edge.x == 700 && edge.y == 550, "and its far corner, the trim's far corner");
    const auto outside = Pointer::ClientPoint(whole, 1.2f, -0.1f, 800, 600);
    Check(outside.x == 799 && outside.y == 0, "past the edge: kept inside the client area");
  });

  Scenario("A tap: one press and one release where the finger was; the game stays in front (R1, INJ-6)", [&] {
    Target target;
    Check(target.mWindow != nullptr, "the click target is up");
    auto lease = helper.Input(1, target.mWindow);
    WaitFor([&] {
      const auto status = helper.InputStatusOf(1);
      return status && status->mState != AppHelper::AppHelperService::State::Waiting;
    }, 8s);
    Check(helper.InputStatusOf(1) && helper.InputStatusOf(1)->mState == AppHelper::AppHelperService::State::On,
      "the helper's input mode is on in it");
    Check(Testing::TakeForeground(game), "the game's window is the foreground");
    Pointer::Injector injector;
    const auto panel = target.AsTarget();
    Tap(injector, panel, 0.25f, 0.5f);
    Check(WaitFor([&] { return target.Has("UP"); }, 3s), "the press and release arrived");
    const auto client = target.Client();
    const auto down = target.Fields("DOWN ");
    const auto up = target.Fields("UP ");
    const double x = client.cx * 0.25;
    const double y = client.cy * 0.5;
    Check(down.size() >= 2 && std::abs(down[0] - x) <= 1 && std::abs(down[1] - y) <= 1,
      std::format("pressed at a quarter across, half down ({}, {})", down.size() > 1 ? down[0] : -1, down.size() > 1 ? down[1] : -1));
    Check(up.size() >= 2 && up[0] == down[0] && up[1] == down[1], "released at the same point");
    Check(target.Count("DOWN") == 1 && target.Count("UP") == 1, "exactly one of each");
    Check(target.Has("MOVES"), "moves came before the press");
    Check(GetForegroundWindow() == game, "the game's window is still the foreground");
    const auto stats = injector.GetStats();
    Check(stats.mPresses == 1 && stats.mReleases == 1 && stats.mTimeouts == 0, "counted: one press, one release, no timeouts");
  });

  Scenario("Hover: at most about 30 moves a second (doc 05)", [&] {
    Target target;
    Pointer::Injector injector;
    const auto panel = target.AsTarget();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; std::chrono::steady_clock::now() - start < 1s; ++i) {
      injector.Pointer(1, &panel, At(0.3f + 0.001f * static_cast<float>(i % 50), 0.5f, TouchState::Hovering));
      std::this_thread::sleep_for(11ms);
    }
    Tap(injector, panel, 0.5f, 0.5f);// its log counts moves at the next press
    Check(WaitFor([&] { return target.Has("UP"); }, 3s), "the tap after arrived");
    const auto moves = target.Fields("MOVES ");
    Check(!moves.empty() && moves[0] >= 20 && moves[0] <= 38,
      std::format("about 30 hover moves in the second ({})", moves.empty() ? -1 : moves[0]));
  });

  Scenario("A double tap is a double-click", [&] {
    Target target;
    Pointer::Injector injector;
    const auto panel = target.AsTarget();
    Tap(injector, panel, 0.5f, 0.5f);
    std::this_thread::sleep_for(100ms);
    Tap(injector, panel, 0.502f, 0.501f);// a couple of pixels off, as fingers land
    Check(WaitFor([&] { return target.Count("UP") == 2; }, 3s), "both taps arrived");
    Check(target.Count("DOWN") == 1 && target.Count("DOUBLE") == 1, "a press, then a double-click");
    const auto first = target.Fields("DOWN ");
    const auto second = target.Fields("DOUBLE ");
    Check(first.size() >= 2 && second.size() >= 2 && first[0] == second[0] && first[1] == second[1],
      "at the first tap's point");
    std::this_thread::sleep_for(GetDoubleClickTime() * 1ms + 100ms);
    Tap(injector, panel, 0.5f, 0.5f);
    Check(WaitFor([&] { return target.Count("UP") == 3; }, 3s) && target.Count("DOWN") == 2,
      "a tap after the double-click time: a plain press");
  });

  // Round 11: finger taps are held 110-250 ms and land 5-10 mm apart, so a
  // mouse's rule (press to press, a few pixels) missed most double taps.
  // The panel here is 45 cm wide (Notepad-like), 800 x 600 pixels.
  Scenario("A finger's double tap: timed from the first one's lift, within 15 mm (ADR-034, amended)", [&] {
    Target target;
    Pointer::Injector injector;
    auto panel = target.AsTarget();
    panel.mWidthMeters = 0.45f;
    panel.mHeightMeters = 0.3375f;
    const auto held = DoubleClickTime() / 2;
    const float apart = 0.010f / panel.mWidthMeters;// 10 mm: 18 pixels
    Tap(injector, panel, 0.5f, 0.5f, held);
    std::this_thread::sleep_for(DoubleClickTime() * 7 / 10);
    // Press to press: held + 70% + the hover lead-in, more than the double-click time
    Tap(injector, panel, 0.5f + apart, 0.5f, held);
    Check(WaitFor([&] { return target.Count("UP") == 2; }, 3s), "both taps arrived");
    Check(target.Count("DOWN") == 1 && target.Count("DOUBLE") == 1,
      std::format("a slow finger's double tap, 10 mm apart, is a double-click ({} presses, {} double)",
        target.Count("DOWN"), target.Count("DOUBLE")));
    const auto first = target.Fields("DOWN ");
    const auto second = target.Fields("DOUBLE ");
    Check(first.size() >= 2 && second.size() >= 2 && first[0] == second[0] && first[1] == second[1],
      "at the first tap's point");

    // A third tap straight after starts over: a plain press
    std::this_thread::sleep_for(100ms);
    Tap(injector, panel, 0.5f, 0.5f);
    Check(WaitFor([&] { return target.Count("UP") == 3; }, 3s) && target.Count("DOWN") == 2 && target.Count("DOUBLE") == 1,
      "a third tap straight after: a plain press");
    Check(injector.GetStats().mDoubleClicks == 1, "counted: one double-click");
  });

  Scenario("Not a double tap: 20 mm apart, a long hold first, or too long after the lift", [&] {
    Target target;
    Pointer::Injector injector;
    auto panel = target.AsTarget();
    panel.mWidthMeters = 0.45f;
    panel.mHeightMeters = 0.3375f;
    // 20 mm apart
    Tap(injector, panel, 0.3f, 0.5f);
    std::this_thread::sleep_for(100ms);
    Tap(injector, panel, 0.3f + (0.020f / panel.mWidthMeters), 0.5f);
    Check(WaitFor([&] { return target.Count("UP") == 2; }, 3s) && target.Count("DOWN") == 2 && target.Count("DOUBLE") == 0,
      "two taps 20 mm apart: two presses");
    std::this_thread::sleep_for(DoubleClickTime() + 100ms);

    // A hold longer than the double-click time, then a quick tap on it
    Tap(injector, panel, 0.6f, 0.5f, DoubleClickTime() + 200ms);
    std::this_thread::sleep_for(100ms);
    Tap(injector, panel, 0.6f, 0.5f);
    Check(WaitFor([&] { return target.Count("UP") == 4; }, 3s) && target.Count("DOWN") == 4 && target.Count("DOUBLE") == 0,
      "a long hold, then a tap: two presses");
    std::this_thread::sleep_for(DoubleClickTime() + 100ms);

    // Lifted, then the next tap after the double-click time
    Tap(injector, panel, 0.7f, 0.5f);
    std::this_thread::sleep_for(DoubleClickTime() + 100ms);
    Tap(injector, panel, 0.7f, 0.5f);
    Check(WaitFor([&] { return target.Count("UP") == 6; }, 3s) && target.Count("DOWN") == 6 && target.Count("DOUBLE") == 0,
      "a second tap after the double-click time from the lift: a plain press");
  });

  Scenario("The wheel, at its point", [&] {
    Target target;
    Pointer::Injector injector;
    const auto panel = target.AsTarget();
    injector.Pointer(1, &panel, At(0.5f, 0.5f, TouchState::Hovering));
    injector.Pointer(1, &panel, At(0.5f, 0.5f, TouchState::Hovering, 240));
    injector.Pointer(1, &panel, At(0.5f, 0.6f, TouchState::Hovering, -120));
    Check(WaitFor([&] { return target.Count("WHEEL") == 2; }, 3s), "two wheel messages");
    const auto first = target.Fields("WHEEL ");
    const auto second = target.Fields("WHEEL ", 1);
    const auto client = target.Client();
    Check(first.size() >= 3 && first[0] == 240 && std::abs(first[1] - client.cx * 0.5) <= 1, "240 (two notches) at the point");
    Check(second.size() >= 1 && second[0] == -120, "then -120, in order, unmerged");
    Check(target.Count("DOWN") == 0, "no click");
  });

  Scenario("Leaving the panel lets go and clears hover; the panel going mid-drag lets go (INJ-4, INJ-5)", [&] {
    Target target;
    Pointer::Injector injector;
    const auto panel = target.AsTarget();
    injector.Pointer(1, &panel, At(0.4f, 0.4f, TouchState::Hovering));
    injector.Pointer(1, &panel, At(0.4f, 0.4f, TouchState::Touching));
    injector.Pointer(1, &panel, At(0.45f, 0.4f, TouchState::Touching));
    injector.Pointer(1, &panel, At(0.5f, 0.4f, TouchState::NotNear));
    Check(WaitFor([&] { return target.Has("LEAVE"); }, 3s), "the mouse left it");
    Check(target.Count("DOWN") == 1 && target.Count("UP") == 1, "the held press was let go first");
    // A drag, then the panel goes (removed, capture stopped)
    injector.Pointer(1, &panel, At(0.4f, 0.4f, TouchState::Hovering));
    injector.Pointer(1, &panel, At(0.4f, 0.4f, TouchState::Touching));
    injector.Pointer(1, &panel, At(0.5f, 0.4f, TouchState::Touching));
    injector.Pointer(1, nullptr, At(0.55f, 0.4f, TouchState::Touching));
    Check(WaitFor([&] { return target.Count("UP") == 2; }, 3s), "the panel went: released");
    Check(target.LastButton().starts_with("UP"), "nothing left held");
  });

  Scenario("The injector going away mid-drag posts the release (INJ-4)", [&] {
    Target target;
    {
      Pointer::Injector injector;
      const auto panel = target.AsTarget();
      injector.Pointer(1, &panel, At(0.4f, 0.4f, TouchState::Hovering));
      injector.Pointer(1, &panel, At(0.4f, 0.4f, TouchState::Touching));
      WaitFor([&] { return target.Has("DOWN"); }, 3s);
    }
    Check(WaitFor([&] { return target.Has("UP"); }, 3s), "released when it went");
  });

  Scenario("A slow app (20 ms a move): moves merge, a press waits for no backlog (INJ-1)", [&] {
    Target target {L"20"};
    Pointer::Injector injector;
    const auto panel = target.AsTarget();
    // A drag at 90 updates a second for 1 s: more than the app can take
    injector.Pointer(1, &panel, At(0.2f, 0.5f, TouchState::Hovering));
    injector.Pointer(1, &panel, At(0.2f, 0.5f, TouchState::Touching));
    for (int i = 0; i < 90; ++i) {
      injector.Pointer(1, &panel, At(0.2f + 0.005f * static_cast<float>(i), 0.5f, TouchState::Touching));
      std::this_thread::sleep_for(11ms);
    }
    injector.Pointer(1, &panel, At(0.65f, 0.5f, TouchState::Hovering));
    const double tapAt = NowMilliseconds();
    Tap(injector, panel, 0.7f, 0.5f);
    Check(WaitFor([&] { return target.Count("UP") == 2; }, 5s), "the drag and the tap arrived");
    const auto press = target.Fields("DOWN ", 1);
    const double late = press.size() >= 3 ? press[2] - tapAt : 1e9;
    Check(late < 250, std::format("the tap's press arrived {:.0f} ms after the tap started (no backlog)", late));
    const auto stats = injector.GetStats();
    Check(stats.mMovesMerged > 0, std::format("moves merged ({})", stats.mMovesMerged));
  });

  Scenario("A hung app: release posted, later taps skipped, no burst when it recovers (INJ-3, R4)", [&] {
    Target target {L"--hang-once 3000"};
    Pointer::Injector injector;
    const auto panel = target.AsTarget();
    Tap(injector, panel, 0.3f, 0.3f);// it stops responding for 3 s at this press
    std::this_thread::sleep_for(600ms);
    Tap(injector, panel, 0.6f, 0.3f);
    std::this_thread::sleep_for(300ms);
    Tap(injector, panel, 0.6f, 0.6f);
    // It recovers about 3 s after the first press
    Check(WaitFor([&] { return target.Count("UP") >= 1; }, 6s), "the first tap's release arrived");
    Pump(1000ms);
    Check(target.Count("DOWN") == 1, std::format("the taps while it hung never landed ({} presses)", target.Count("DOWN")));
    Check(target.LastButton().starts_with("UP"), "no stuck button");
    const auto stats = injector.GetStats();
    Check(stats.mSkippedHung > 0, std::format("skipped while it hung ({})", stats.mSkippedHung));
    // Responding again: taps work
    Tap(injector, panel, 0.5f, 0.5f);
    Check(WaitFor([&] { return target.Count("DOWN") == 2 && target.Count("UP") >= 2; }, 3s), "after it recovers, a tap lands");
    Check(injector.GetStats().mSkippedPresses == 2,
      std::format("counted: the two taps while it hung ({})", injector.GetStats().mSkippedPresses));

    // Round 11: the log lines name the app, and say when it answers again
    Log::Stop();
    std::string log;
    {
      std::ifstream file(gLogFile);
      std::stringstream text;
      text << file.rdbuf();
      log = text.str();
    }
    Check(log.contains("Input: click-target.exe took more than 250 ms"), "the timeout's log line names the app");
    Check(log.contains("Input: click-target.exe isn't responding"), "so does the not-responding line");
    Check(log.contains("Input: click-target.exe is answering again") && log.contains("skipped meanwhile: 2 taps"),
      "and a line says when it answered again, with the two taps it skipped");
  });

  DestroyWindow(game);
  Log::Stop();
  std::error_code ignored;
  std::filesystem::remove(gLogFile, ignored);
  std::filesystem::remove(std::filesystem::path {gLogFile}.replace_extension(L".previous.log"), ignored);
  return Testing::Finish();
}
