// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The injection matrix's mouse rows (doc 11) on real browsers, Firefox and
// Edge on throwaway profiles, each fully covered by this test's window with
// the foreground (the game), with the app helper loaded as the host loads it
// (painting and input):
// - tap a button: one click;
// - double-tap it: a double-click;
// - drag a slider: it follows, and ends on release;
// - the wheel over a scrolling box: it scrolls;
// - throughout, the browser stays behind the cover and the foreground stays
//   the cover's (R1).
// The page reports what happened through its title. A browser that isn't
// installed is skipped.

#include "browser.hpp"
#include "check.hpp"
#include "foreground.hpp"

#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

using namespace OpenDesktopVR;
using namespace std::chrono_literals;
using AppHelper::AppHelperService;
using HandInteraction::CursorUpdate;
using HandInteraction::TouchState;
using Testing::Check;
using Testing::Scenario;

namespace {

// A big button (top left), a scrolling box (top right), a wide slider
constexpr char Page[] = R"(<!doctype html>
<title>odvr init</title>
<body style="margin:0;background:#202830;overflow:hidden">
<button id="b" style="position:absolute;left:0;top:0;width:50vw;height:40vh;font-size:24px">Tap</button>
<div id="d" style="position:absolute;left:55vw;top:0;width:40vw;height:40vh;overflow-y:scroll;background:#334">
<div style="height:4000px;color:#ccc">scroll</div></div>
<input id="s" type="range" min="0" max="100" value="0"
 style="position:absolute;left:5vw;top:55vh;width:90vw;height:8vh;margin:0">
<script>
let clicks = 0, doubles = 0, moves = 0, downs = 0;
b.addEventListener('click', () => clicks++);
b.addEventListener('dblclick', () => doubles++);
document.addEventListener('mousemove', () => moves++);
document.addEventListener('mousedown', () => downs++);
setInterval(() => {
  document.title = `odvr c ${clicks} d ${doubles} s ${s.value} y ${Math.round(d.scrollTop)} w ${innerWidth} h ${innerHeight} m ${moves} p ${downs}`;
}, 50);
</script></body>
)";

// "odvr c 1 d 0 s 0 y 0 w 900 h 600 - Mozilla Firefox"
std::map<std::string, long> ReadPage(HWND window) {
  const auto title = Testing::TitleOf(window);
  std::map<std::string, long> fields;
  if (!title.starts_with(L"odvr c ")) {
    return fields;
  }
  // The title is ASCII up to the browser's name
  std::string text;
  for (const auto c: title) {
    text.push_back(c < 128 ? static_cast<char>(c) : '?');
  }
  std::istringstream in(text.substr(5));
  std::string key;
  long value = 0;
  while (in >> key >> value) {
    fields[key] = value;
  }
  return fields;
}

long Field(HWND window, const char* key) {
  const auto fields = ReadPage(window);
  const auto it = fields.find(key);
  return it == fields.end() ? -1 : it->second;
}

// The page's CSS pixels to a point on the "panel" (the whole client area):
// the page's viewport sits at the bottom of the client area, centred, below
// the browser's own toolbars
struct PageMap {
  HWND mWindow;
  double mScale;
  double mLeft;
  double mTop;
  LONG mWidth;
  LONG mHeight;

  static std::optional<PageMap> Of(HWND window) {
    const auto fields = ReadPage(window);
    if (!fields.contains("w") || !fields.contains("h")) {
      return std::nullopt;
    }
    RECT client {};
    GetClientRect(window, &client);
    const double scale = GetDpiForWindow(window) / 96.0;
    const double width = static_cast<double>(fields.at("w")) * scale;
    const double height = static_cast<double>(fields.at("h")) * scale;
    return PageMap {window, scale, (client.right - width) / 2, client.bottom - height, client.right, client.bottom};
  }
  // u, v for a point in CSS pixels
  std::pair<float, float> UV(double x, double y) const {
    return {static_cast<float>((mLeft + x * mScale) / mWidth), static_cast<float>((mTop + y * mScale) / mHeight)};
  }
  Pointer::Target Target() const {
    return {mWindow, {0, 0, mWidth, mHeight}, {mWidth, mHeight}};
  }
};

CursorUpdate At(std::pair<float, float> uv, TouchState state, int32_t wheel = 0) {
  return {.mPanelID = 1, .mU = uv.first, .mV = uv.second, .mState = state, .mWheel = wheel};
}

void Step(Pointer::Injector& injector, const Pointer::Target& target, std::pair<float, float> uv, TouchState state, int times = 1) {
  for (int i = 0; i < times; ++i) {
    injector.Pointer(1, &target, At(uv, state));
    Testing::PumpFor(11ms);
  }
}

void Tap(Pointer::Injector& injector, const Pointer::Target& target, std::pair<float, float> uv) {
  Step(injector, target, uv, TouchState::Hovering, 3);
  Step(injector, target, uv, TouchState::Touching, 3);
  Step(injector, target, uv, TouchState::Hovering);
}

LRESULT CALLBACK CoverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  return DefWindowProcW(window, message, wParam, lParam);
}

// The game: an ordinary opaque window over the browser, with the foreground
HWND Cover(HWND target) {
  static const ATOM registered = [] {
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = &CoverProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
    windowClass.lpszClassName = L"OdvrBrowserInputTestGame";
    return RegisterClassW(&windowClass);
  }();
  (void)registered;
  RECT rect {};
  GetWindowRect(target, &rect);
  const HWND cover = CreateWindowExW(0, L"OdvrBrowserInputTestGame", L"browser-input-test (the game)",
    WS_OVERLAPPED | WS_VISIBLE, rect.left - 20, rect.top - 20, rect.right - rect.left + 40, rect.bottom - rect.top + 40,
    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Testing::TakeForeground(cover);
  return cover;
}

bool Above(HWND upper, HWND lower) {
  for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
    if (window == upper) {
      return true;
    }
    if (window == lower) {
      return false;
    }
  }
  return false;
}

bool IsOn(const std::function<std::optional<AppHelperService::Status>()>& status) {
  Testing::WaitUntil([&] {
    const auto now = status();
    return now && now->mState != AppHelperService::State::Waiting;
  }, 10s);
  const auto now = status();
  return now && now->mState == AppHelperService::State::On;
}

void Run(const Testing::BrowserKind& kind) {
  if (!std::filesystem::exists(kind.mExe)) {
    std::printf("  (skipped: %s isn't installed here)\n", kind.mName);
    return;
  }
  Testing::TempFolder folder;
  const auto profile = (std::filesystem::path {folder.Path()} / L"profile").wstring();
  const auto pagePath = std::filesystem::path {folder.Path()} / L"page.html";
  std::filesystem::create_directories(profile);
  if (kind.mPrepareProfile) {
    kind.mPrepareProfile(profile);
  }
  std::ofstream {pagePath} << Page;
  const std::wstring url = L"file:///" + pagePath.generic_wstring();

  AppHelperService service;
  Testing::Browser browser {kind.mExe, kind.mArguments(profile, url)};
  if (!Check(browser.Started(), std::format("{} starts", kind.mName))) {
    return;
  }
  const HWND window = browser.FindPage(30s);
  if (!Check(window != nullptr, "its page's window appears")) {
    return;
  }
  SetWindowPos(window, nullptr, 80, 80, 1000, 760, SWP_NOZORDER | SWP_NOACTIVATE);
  Testing::TakeForeground(window);
  Check(Testing::WaitUntil([&] { return Field(window, "c") == 0 && PageMap::Of(window); }, 10s), "the page is ready");
  Testing::PumpFor(500ms);// the resize settles
  const HWND cover = Cover(window);
  Check(GetForegroundWindow() == cover, "the game's window has the foreground, over the browser");

  // As the host loads it: painting and input, from capture start
  auto painting = service.KeepPainting(1, window);
  auto input = service.Input(1, window);
  Check(IsOn([&] { return service.StatusOf(1); }), "the helper keeps it painting");
  Check(IsOn([&] { return service.InputStatusOf(1); }), "the helper's input mode is on");
  const auto map = PageMap::Of(window);
  if (!Check(map.has_value(), "the page reports its size")) {
    DestroyWindow(cover);
    return;
  }
  const auto target = map->Target();
  Pointer::Injector injector;
  const auto css = [&](double x, double y) { return map->UV(x, y); };
  const auto fields = ReadPage(window);
  const double vw = static_cast<double>(fields.at("w")) / 100.0;
  const double vh = static_cast<double>(fields.at("h")) / 100.0;

  // Tap the button
  Tap(injector, target, css(25 * vw, 20 * vh));
  Check(Testing::WaitUntil([&] { return Field(window, "c") == 1; }, 3s),
    std::format("a tap on the button: one click ({}; the page saw {} moves, {} presses)", Field(window, "c"),
      Field(window, "m"), Field(window, "p")));

  // Double-tap it
  Testing::PumpFor(std::chrono::milliseconds(GetDoubleClickTime() + 100));
  Tap(injector, target, css(25 * vw, 20 * vh));
  Testing::PumpFor(80ms);
  Tap(injector, target, css(25 * vw + 2, 20 * vh + 1));
  Check(Testing::WaitUntil([&] { return Field(window, "d") == 1; }, 3s),
    std::format("a double tap: a double-click ({} double, {} clicks)", Field(window, "d"), Field(window, "c")));

  // Drag the slider from its left end three quarters across
  const double sliderY = 59 * vh;
  const double left = 5 * vw + 4;
  const double right = 5 * vw + 0.75 * 90 * vw;
  Step(injector, target, css(left, sliderY), TouchState::Hovering, 3);
  Step(injector, target, css(left, sliderY), TouchState::Touching, 3);
  for (int i = 1; i <= 30; ++i) {
    Step(injector, target, css(left + (right - left) * i / 30.0, sliderY), TouchState::Touching);
  }
  Step(injector, target, css(right, sliderY), TouchState::Hovering);
  Check(Testing::WaitUntil([&] { return Field(window, "s") >= 60; }, 3s),
    std::format("a drag moves the slider ({} of 100)", Field(window, "s")));
  // After the release, moving over it changes nothing
  Testing::PumpFor(300ms);// the drag's last moves land
  const auto released = Field(window, "s");
  Step(injector, target, css(left, sliderY), TouchState::Hovering, 5);
  Testing::PumpFor(300ms);
  Check(Field(window, "s") == released, "and it ends on release");

  // The wheel over the scrolling box: down three notches
  const auto box = css(75 * vw, 20 * vh);
  Step(injector, target, box, TouchState::Hovering, 3);
  injector.Pointer(1, &target, At(box, TouchState::Hovering, -360));
  Check(Testing::WaitUntil([&] { return Field(window, "y") > 0; }, 3s),
    std::format("the wheel scrolls the box under the point ({} px)", Field(window, "y")));
  injector.Pointer(1, &target, At(box, TouchState::NotNear));

  Testing::PumpFor(300ms);
  Check(GetForegroundWindow() == cover, "the game's window kept the foreground throughout");
  Check(Above(cover, window), "the browser stayed behind it");
  const auto stats = injector.GetStats();
  Check(stats.mTimeouts == 0, std::format("no message timed out ({})", stats.mTimeouts));
  DestroyWindow(cover);
}

}// namespace

int main() {
  for (const auto& kind: Testing::Browsers()) {
    Scenario(std::format("{}: tap, double-tap, slider, wheel, while covered (doc 11)", kind.mName).c_str(), [&] { Run(kind); });
  }
  return Testing::Finish();
}
