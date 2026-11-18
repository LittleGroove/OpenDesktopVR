// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The injection matrix's keyboard rows (doc 11, M5a) on Firefox and Edge (a
// textarea, throwaway profiles) and a Win32 EDIT (edit-target-app), each
// fully covered by this test's window with the foreground (the game), with
// the app helper loaded as the host loads it:
// - the keyboard's auto-show (M5b, doc 11): in the browsers, a tap on a
//   button whose page then focuses the text field far from the tap doesn't
//   show it (K3); then a tap into the text field gives it the caret, and the
//   keyboard shows within 1.5 s (VK-1, VK-2), by the keyboard service's own
//   rules on the real caret monitor;
// - "Hello", the H with Shift;
// - Enter: exactly one newline (Firefox once typed Enter twice: INJ-9);
// - German: dead key ´ then e is é, and ü: the characters from the layout's
//   tables, whatever the app's own layout is;
// - Ctrl+A: all selected in the browsers; in the EDIT nothing is typed
//   (Win32 EDIT controls don't support it: expected, doc 06);
// - Alt+F4 and Alt+Tab: never delivered (INJ-8);
// - after each step, the game has the foreground and the app is behind it
//   (R1). Windows decides whether an app may take the foreground when it
//   activates (the EDIT does on a click; the helper makes a window active
//   for keys). Where it may, as for apps this test starts on many PCs, the
//   helper gives it back (ADR-035) and the output says so; in VR it isn't
//   taken at all (test round 11).
// A browser that isn't installed is skipped.
//
//   key-inject-test <edit-target-app exe>

#include "browser.hpp"
#include "check.hpp"
#include "foreground.hpp"

#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/KeyboardAutoShow.hpp>
#include <OpenDesktopVR/KeyboardLayout.hpp>
#include <OpenDesktopVR/PointerInjector.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/TextInputMonitor.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
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

// A textarea; the title says what the page sees: key-downs, Enter key-downs,
// the selection's length, whether the textarea has the focus, the viewport,
// and the text (percent-encoded, in brackets)
constexpr char Page[] = R"(<!doctype html>
<title>odvr init</title>
<body style="margin:0;background:#202830;overflow:hidden">
<textarea id="t" style="position:absolute;left:5vw;top:5vh;width:90vw;height:40vh;font-size:24px;margin:0"></textarea>
<button id="b" style="position:absolute;left:40vw;top:80vh;width:20vw;height:10vh;font-size:24px">Focus</button>
<script>
b.addEventListener('click', () => t.focus());
let downs = 0, enters = 0;
t.addEventListener('keydown', e => { downs++; if (e.key === 'Enter') enters++; });
setInterval(() => {
  document.title = 'odvr k ' + downs + ' n ' + enters + ' s ' + (t.selectionEnd - t.selectionStart)
    + ' f ' + (document.activeElement === t ? 1 : 0) + ' w ' + innerWidth + ' h ' + innerHeight
    + ' v [' + encodeURIComponent(t.value) + ']';
}, 50);
</script></body>
)";

std::wstring FromUtf8(const std::string& bytes) {
  if (bytes.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
  std::wstring text(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
  return text;
}

std::wstring PercentDecode(std::string_view encoded) {
  std::string bytes;
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      bytes.push_back(static_cast<char>(std::stoi(std::string {encoded.substr(i + 1, 2)}, nullptr, 16)));
      i += 2;
    } else {
      bytes.push_back(encoded[i]);
    }
  }
  return FromUtf8(bytes);
}

struct PageState {
  std::map<std::string, long> mFields;
  std::wstring mText;
};

// "odvr k 3 n 1 s 0 f 1 w 900 h 600 v [Hello%0A] - Mozilla Firefox"
std::optional<PageState> ReadPage(HWND window) {
  const auto title = Testing::TitleOf(window);
  if (!title.starts_with(L"odvr k ")) {
    return std::nullopt;
  }
  std::string text;
  for (const auto c: title) {
    text.push_back(c < 128 ? static_cast<char>(c) : '?');
  }
  const auto open = text.find(" v [");
  const auto close = text.find(']', open);
  if (open == std::string::npos || close == std::string::npos) {
    return std::nullopt;
  }
  PageState state;
  state.mText = PercentDecode(std::string_view {text}.substr(open + 4, close - open - 4));
  std::istringstream in(text.substr(5, open - 5));
  std::string key;
  long value = 0;
  while (in >> key >> value) {
    state.mFields[key] = value;
  }
  return state;
}

long Field(HWND window, const char* key) {
  const auto state = ReadPage(window);
  if (!state || !state->mFields.contains(key)) {
    return -1;
  }
  return state->mFields.at(key);
}

LRESULT CALLBACK CoverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  return DefWindowProcW(window, message, wParam, lParam);
}

// The game: an ordinary opaque window over the app, with the foreground
HWND Cover(HWND target) {
  static const ATOM registered = [] {
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = &CoverProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
    windowClass.lpszClassName = L"OdvrKeyInjectTestGame";
    return RegisterClassW(&windowClass);
  }();
  (void)registered;
  RECT rect {};
  GetWindowRect(target, &rect);
  const HWND cover = CreateWindowExW(0, L"OdvrKeyInjectTestGame", L"key-inject-test (the game)", WS_OVERLAPPED | WS_VISIBLE,
    rect.left - 20, rect.top - 20, rect.right - rect.left + 40, rect.bottom - rect.top + 40, nullptr, nullptr,
    GetModuleHandleW(nullptr), nullptr);
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

// The step during which the foreground first left the game (R1): a WinEvent
// hook, called on this thread while the test pumps messages
class ForegroundWatch {
 public:
  ForegroundWatch() {
    sGame = nullptr;
    sStep = "the game's start";
    sLost.clear();
    sHistory.clear();
    sChanges = 0;
    mHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Changed, 0, 0, WINEVENT_OUTOFCONTEXT);
  }
  ~ForegroundWatch() {
    if (mHook) {
      UnhookWinEvent(mHook);
    }
  }
  ForegroundWatch(const ForegroundWatch&) = delete;
  ForegroundWatch& operator=(const ForegroundWatch&) = delete;

  static void Step(const char* step) {
    sStep = step;
  }
  // From here on, the foreground should stay with `game`
  static void Watch(HWND game) {
    sGame = game;
    sLost.clear();
  }
  // Empty while the game has kept it
  static const std::string& Lost() {
    return sLost;
  }
  // Every change, for a failure's output (the first 12)
  static const std::string& History() {
    return sHistory;
  }

 private:
  static void CALLBACK Changed(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    wchar_t name[128] {};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (++sChanges <= 12) {
      sHistory += std::format("\n      during {}: {}", sStep, window == sGame ? std::string {"the game"} : StringUtil::Narrow(name));
    }
    if (sGame && window != sGame && sLost.empty()) {
      sLost = std::format("lost during {}, to a {} window", sStep, StringUtil::Narrow(name));
    }
  }

  static inline HWND sGame {nullptr};
  static inline const char* sStep {""};
  static inline std::string sLost;
  static inline std::string sHistory;
  static inline int sChanges {0};
  HWINEVENTHOOK mHook {nullptr};
};

bool IsOn(const std::function<std::optional<AppHelperService::Status>()>& status) {
  Testing::WaitUntil([&] {
    const auto now = status();
    return now && now->mState != AppHelperService::State::Waiting;
  }, 10s);
  const auto now = status();
  return now && now->mState == AppHelperService::State::On;
}

void Tap(Pointer::Injector& injector, const Pointer::Target& target, std::pair<float, float> uv) {
  const auto at = [&](TouchState state) {
    return CursorUpdate {.mPanelID = 1, .mU = uv.first, .mV = uv.second, .mState = state};
  };
  for (int i = 0; i < 3; ++i) {
    injector.Pointer(1, &target, at(TouchState::Hovering));
    Testing::PumpFor(11ms);
  }
  for (int i = 0; i < 3; ++i) {
    injector.Pointer(1, &target, at(TouchState::Touching));
    Testing::PumpFor(11ms);
  }
  injector.Pointer(1, &target, at(TouchState::Hovering));
  Testing::PumpFor(11ms);
  injector.Pointer(1, &target, at(TouchState::NotNear));
}

// Key presses as the virtual keyboard will make them (M5b): the key's
// virtual key and characters from the layout's tables
struct Keys {
  std::shared_ptr<const KeyboardLayout::Layout> mUS {KeyboardLayout::Layout::Load("00000409")};
  std::shared_ptr<const KeyboardLayout::Layout> mGerman {KeyboardLayout::Layout::Load("00000407")};

  VirtualKeyStroke Key(KeyboardLayout::ScanCode scan, bool shift = false) const {
    return {scan, mUS->GetVirtualKey(scan), mUS->GetOutput(scan, {.mShift = shift}).mText, shift};
  }
  static VirtualKeyStroke Enter() {
    return {0x1C, VK_RETURN, L"\r"};
  }
  static VirtualKeyStroke ControlA() {
    return {.mScanCode = 0x1E, .mVirtualKey = 'A', .mControl = true};
  }
  static VirtualKeyStroke AltF4() {
    return {.mScanCode = 0x3E, .mVirtualKey = VK_F4, .mAlt = true};
  }
  static VirtualKeyStroke AltTab() {
    return {.mScanCode = 0x0F, .mVirtualKey = VK_TAB, .mAlt = true};
  }
};

// What the test needs from a target: its window, where its text field is,
// and what it holds now
struct TextTarget {
  HWND mWindow {nullptr};
  Pointer::Target mTarget {};
  std::pair<float, float> mField {0.5f, 0.5f};
  // A button whose page then focuses the field by itself (browsers)
  std::optional<std::pair<float, float>> mButton;
  std::function<std::wstring()> mText;
  std::function<long()> mSelected;// selection length
  std::function<bool()> mFocused;
  std::wstring mNewline;
  bool mSelectAll {true};// Ctrl+A selects all (not the Win32 EDIT)
};

// The keyboard service's auto-show rules (VK-1, VK-2, K3) on the real caret
// monitor, as the host runs them
class AutoShowWatch {
 public:
  void Tapped(const Pointer::Target& target, std::pair<float, float> uv) {
    RECT client {};
    GetClientRect(target.mWindow, &client);
    POINT screen = Pointer::ClientPoint(target, uv.first, uv.second, client.right, client.bottom);
    ClientToScreen(target.mWindow, &screen);
    {
      std::scoped_lock lock(mMutex);
      mAutoShow.Tapped(Seconds(), target.mWindow, screen);
      mTap = screen;
    }
    mMonitor.SetWindow(target.mWindow);
    for (const auto delay: VirtualKeyboard::AutoShow::TapChecks) {
      mMonitor.Poke(delay, true);
    }
  }
  bool Shown() {
    std::scoped_lock lock(mMutex);
    return mAutoShow.IsShown();
  }
  std::string Why() {
    std::scoped_lock lock(mMutex);
    return mWhy;
  }
  // What the monitor last said, and where the tap was
  std::string Last() {
    std::scoped_lock lock(mMutex);
    std::string caret = "none";
    if (mLast.mCaret) {
      caret = std::format("({}, {}, {}, {})", mLast.mCaret->left, mLast.mCaret->top, mLast.mCaret->right, mLast.mCaret->bottom);
    }
    return std::format("{} reports; typing {}, caret {}; the tap at ({}, {})", mReports, mLast.mTyping ? "yes" : "no", caret,
      mTap.x, mTap.y);
  }

 private:
  static double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  std::mutex mMutex;
  VirtualKeyboard::AutoShow mAutoShow;
  std::string mWhy;
  TextInputMonitor::State mLast;
  int mReports {0};
  POINT mTap {};
  // Last: its thread calls back into the members above
  TextInputMonitor mMonitor {[this](HWND window, const TextInputMonitor::State& state) {
    std::scoped_lock lock(mMutex);
    mLast = state;
    ++mReports;
    if (const auto change = mAutoShow.Caret(Seconds(), window, state.mTyping, state.mCaret)) {
      mWhy = change->mWhy;
    }
  }};
};

void TypeAll(const char* name, const TextTarget& field, AppHelperService& service) {
  const ForegroundWatch watch;
  const HWND cover = Cover(field.mWindow);
  Check(GetForegroundWindow() == cover, "the game's window has the foreground, over the app");
  ForegroundWatch::Watch(cover);
  ForegroundWatch::Step("the helper's start");
  auto input = service.Input(1, field.mWindow);
  Check(IsOn([&] { return service.InputStatusOf(1); }), "the helper's input mode is on");

  Pointer::Injector injector;
  const Keys keys;
  if (!Check(keys.mUS && keys.mGerman, "the US and German layouts load")) {
    DestroyWindow(cover);
    return;
  }
  const auto type = [&](const VirtualKeyStroke& stroke) { injector.Key(field.mTarget, stroke); };
  // R1 after each step: the game has the foreground, given back if the app
  // took it, and the app is behind it
  const auto gameKept = [&](const char* step) {
    const bool back = Testing::WaitUntil([&] { return GetForegroundWindow() == cover; }, 1s);
    Check(back && Above(cover, field.mWindow), std::format("after {} the game has the foreground, over the app", step));
  };

  AutoShowWatch autoShow;
  if (field.mButton) {
    // K3: the page focusing its field by itself, after a tap far from it
    // (Discord on a channel switch), doesn't show the keyboard
    ForegroundWatch::Step("the button");
    Tap(injector, field.mTarget, *field.mButton);
    autoShow.Tapped(field.mTarget, *field.mButton);
    const bool focusedByPage = Testing::WaitUntil(field.mFocused, 3s);
    Testing::PumpFor(1600ms);// past the 1.5 s a tap can show it
    if (focusedByPage) {
      Check(!autoShow.Shown(), std::format("{}: the page focusing its field by itself didn't show the keyboard (K3)", name));
    } else {
      std::printf("  (%s: the page's field didn't take the focus after the button)\n", name);
    }
    gameKept("the button");
  }

  ForegroundWatch::Step("the tap");
  Tap(injector, field.mTarget, field.mField);
  autoShow.Tapped(field.mTarget, field.mField);
  const bool focused = Testing::WaitUntil(field.mFocused, 3s);
  // VK-1, VK-2: the caret near the tap within 1.5 s shows the keyboard.
  // Where the tap's activation made the app take the foreground, giving it
  // back took the app's focus, and its caret, with it (ADR-035): only VR
  // (where it isn't taken) can show this
  const bool shown = Testing::WaitUntil([&] { return autoShow.Shown(); }, 1500ms);
  if (shown || ForegroundWatch::Lost().empty()) {
    Check(shown, std::format("{}: a tap into the text field shows the keyboard within 1.5 s ({})", name,
      shown ? autoShow.Why() : autoShow.Last()));
  } else {
    std::printf("  (%s: no keyboard: the tap made the app take the foreground, and giving it back took its caret; %s)\n",
      name, autoShow.Last().c_str());
  }
  if (focused || ForegroundWatch::Lost().empty()) {
    Check(focused, std::format("{}: a tap into the text field gives it the focus", name));
  } else {
    // It activated, took the foreground, gave it back, and lost its focus
    // with it: keys go to the control tapped (ADR-035)
    std::printf("  (%s: the tap made it take the foreground; given back, it has no focus)\n", name);
  }
  gameKept("the tap");

  // "Hello", the H with Shift (the keyboard's latch sends it with Shift set)
  ForegroundWatch::Step("\"Hello\"");
  type(keys.Key(0x23, true));
  for (const KeyboardLayout::ScanCode scan: std::initializer_list<KeyboardLayout::ScanCode> {0x12, 0x26, 0x26, 0x18}) {
    type(keys.Key(scan));
  }
  const bool hello = Testing::WaitUntil([&] { return field.mText() == L"Hello"; }, 3s);
  Check(hello, std::format("{}: typed \"Hello\" (it has \"{}\")", name, StringUtil::Narrow(field.mText())));
  gameKept("\"Hello\"");

  // Enter: exactly one newline, still one a moment later
  ForegroundWatch::Step("Enter");
  type(Keys::Enter());
  const auto afterEnter = L"Hello" + field.mNewline;
  Testing::WaitUntil([&] { return field.mText() == afterEnter; }, 3s);
  Testing::PumpFor(500ms);
  Check(field.mText() == afterEnter,
    std::format("{}: Enter typed exactly one newline (it has \"{}\")", name, StringUtil::Narrow(field.mText())));

  // German: ´ then e is é; ü
  KeyboardLayout::Typist typist {keys.mGerman};
  const auto dead = typist.Press(0x0D, {});
  const auto composed = typist.Press(0x12, {});
  Check(dead.mText.empty() && dead.mPendingDeadKey && composed.mText == L"é",
    "German layout: ´ waits, then e composes é");
  ForegroundWatch::Step("German é and ü");
  type({0x12, keys.mGerman->GetVirtualKey(0x12), composed.mText});
  const auto umlaut = keys.mGerman->GetOutput(0x1A, {});
  type({0x1A, keys.mGerman->GetVirtualKey(0x1A), umlaut.mText});
  const auto afterGerman = afterEnter + L"éü";
  const bool german = Testing::WaitUntil([&] { return field.mText() == afterGerman; }, 3s);
  Check(german, std::format("{}: German é and ü arrived as typed (it has \"{}\")", name, StringUtil::Narrow(field.mText())));

  // Ctrl+A
  ForegroundWatch::Step("Ctrl+A");
  type(Keys::ControlA());
  if (field.mSelectAll) {
    const bool all = Testing::WaitUntil([&] { return field.mSelected() == static_cast<long>(field.mText().size()); }, 3s);
    Check(all, std::format("{}: Ctrl+A selected all ({} of {} characters)", name, field.mSelected(), field.mText().size()));
  } else {
    Testing::PumpFor(300ms);
    std::printf("  (%s: Ctrl+A selected %ld characters: Win32 EDIT controls may not support it, expected)\n", name,
      field.mSelected());
  }
  Check(field.mText() == afterGerman, std::format("{}: Ctrl+A typed nothing", name));

  // Blocked: Alt+F4 would close it, Alt+Tab switch away from the game
  ForegroundWatch::Step("the blocked combinations");
  type(Keys::AltF4());
  type(Keys::AltTab());
  Testing::PumpFor(700ms);
  Check(IsWindow(field.mWindow) && IsWindowVisible(field.mWindow), std::format("{}: Alt+F4 never reached it (still open)", name));
  const auto stats = injector.GetStats();
  Check(stats.mKeysBlocked == 2, std::format("both combinations were held back ({})", stats.mKeysBlocked));
  Check(stats.mKeys == 9, std::format("the other 9 keys went ({})", stats.mKeys));
  Check(stats.mTimeouts == 0, std::format("no message timed out ({})", stats.mTimeouts));
  gameKept("all the keys");
  // Whether the app took the foreground at all is Windows' choice: on a PC
  // that lets any app take it (this test's apps, started from a foreground
  // terminal, often may) it's given back; in VR it's never taken (round 11)
  if (!ForegroundWatch::Lost().empty()) {
    std::printf("  (the app took the foreground and gave it back: %s; changes:%s)\n", ForegroundWatch::Lost().c_str(),
      ForegroundWatch::History().c_str());
  }
  DestroyWindow(cover);
}

void RunBrowser(const Testing::BrowserKind& kind) {
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
  Check(Testing::WaitUntil([&] { return Field(window, "w") > 0; }, 10s), "the page is ready");
  Testing::PumpFor(500ms);// the resize settles
  auto painting = service.KeepPainting(1, window);
  Check(IsOn([&] { return service.StatusOf(1); }), "the helper keeps it painting");

  // The page's viewport sits at the bottom of the client area, centred
  RECT client {};
  GetClientRect(window, &client);
  const double scale = GetDpiForWindow(window) / 96.0;
  const double width = Field(window, "w") * scale;
  const double height = Field(window, "h") * scale;
  const double left = (client.right - width) / 2;
  const double top = client.bottom - height;
  // The textarea's first line: 50vw, 8vh (its caret goes there when it's
  // empty: near the tap, as in a chat app's one-line box, VK-1)
  const auto u = static_cast<float>((left + width * 0.5) / client.right);
  const auto v = static_cast<float>((top + height * 0.08) / client.bottom);

  TextTarget field;
  field.mWindow = window;
  field.mTarget = {window, {0, 0, client.right, client.bottom}, {client.right, client.bottom}};
  field.mField = {u, v};
  // The button's middle: 50vw, 85vh
  field.mButton = std::pair {u, static_cast<float>((top + height * 0.85) / client.bottom)};
  field.mText = [window] {
    const auto state = ReadPage(window);
    return state ? state->mText : std::wstring {L"?"};
  };
  field.mSelected = [window] { return Field(window, "s"); };
  field.mFocused = [window] { return Field(window, "f") == 1; };
  field.mNewline = L"\n";
  TypeAll(kind.mName, field, service);
  Check(Field(window, "n") == 1, std::format("{}: the page saw one Enter key-down ({})", kind.mName, Field(window, "n")));
}

void RunEdit(const std::filesystem::path& exe) {
  STARTUPINFOW startup {sizeof(startup)};
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_SHOWNOACTIVATE;
  PROCESS_INFORMATION process {};
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  if (!Check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != 0,
        "edit-target-app starts")) {
    return;
  }
  HWND window = nullptr;
  Testing::WaitUntil([&] {
    window = FindWindowW(L"OdvrEditTarget", nullptr);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    return window && owner == process.dwProcessId;
  }, 5s);
  const HWND edit = window ? GetDlgItem(window, 1) : nullptr;
  if (Check(edit != nullptr, "its window and EDIT appear")) {
    RECT client {};
    GetClientRect(window, &client);
    AppHelperService service;
    TextTarget field;
    field.mWindow = window;
    field.mTarget = {window, {0, 0, client.right, client.bottom}, {client.right, client.bottom}};
    // Its first line: where an empty EDIT's caret goes (near the tap, VK-1)
    field.mField = {0.5f, 0.02f};
    field.mText = [edit] {
      wchar_t text[256] {};
      DWORD_PTR copied = 0;
      SendMessageTimeoutW(edit, WM_GETTEXT, std::size(text), reinterpret_cast<LPARAM>(text), SMTO_ABORTIFHUNG, 1000, &copied);
      return std::wstring {text};
    };
    field.mSelected = [edit] {
      DWORD_PTR selection = 0;
      SendMessageTimeoutW(edit, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 1000, &selection);
      return static_cast<long>(HIWORD(selection)) - static_cast<long>(LOWORD(selection));
    };
    field.mFocused = [window, edit] {
      GUITHREADINFO info {.cbSize = sizeof(info)};
      return GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info) && info.hwndFocus == edit;
    };
    field.mNewline = L"\r\n";
    field.mSelectAll = false;
    TypeAll("Win32 EDIT", field, service);
  }
  TerminateProcess(process.hProcess, 0);
  WaitForSingleObject(process.hProcess, 5000);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
}

}// namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("Usage: key-inject-test <edit-target-app exe>\n");
    return 2;
  }
  const auto editTarget = std::filesystem::absolute(argv[1]);
  Scenario("Win32 EDIT: tap, type, Enter, German, Ctrl+A, blocked combinations, while covered (doc 11)",
    [&] { RunEdit(editTarget); });
  for (const auto& kind: Testing::Browsers()) {
    Scenario(std::format("{}: tap, type, Enter, German, Ctrl+A, blocked combinations, while covered (doc 11)", kind.mName).c_str(),
      [&] { RunBrowser(kind); });
  }
  return Testing::Finish();
}
