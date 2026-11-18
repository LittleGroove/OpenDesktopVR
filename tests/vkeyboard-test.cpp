// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The virtual keyboard's logic (M5b, doc 07, ADR-036), without a headset:
// - geometry: ANSI and ISO, with and without the function row; rows add
//   up, keys never overlap, every key is hit at its middle, nothing reaches
//   Windows (no Windows, Menu, Print Screen, Scroll Lock or Pause key), and
//   the panel is wide enough for the simulator to tell it apart;
// - keys on the US and German layouts: Shift latches and locks (400 ms),
//   Caps Lock, Ctrl+letter's control character, Alt combinations without
//   text (Alt+F4 still blocked), AltGr, dead keys, Num Lock on the numpad,
//   Enter, Backspace, the layout and hide keys, legends following Shift;
// - repeat: Windows' delay and rate, never faster than 50 ms (VK-3), no
//   burst after a stall, not for Enter, stopped by the release;
// - when it shows and hides (VK-1, VK-2, K3 to K6);
// - the shared model: hover changes only count when they change, a key
//   press is urgent, and every change wakes the renderer.

#include "check.hpp"

#include <OpenDesktopVR/KeyboardAutoShow.hpp>
#include <OpenDesktopVR/KeyboardGeometry.hpp>
#include <OpenDesktopVR/KeyboardModel.hpp>
#include <OpenDesktopVR/VirtualKeyboard.hpp>
#include <OpenDesktopVR/WindowKeyInjection.hpp>

#include <Windows.h>

#include <cmath>
#include <format>
#include <string>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::VirtualKeyboard;
using Testing::Check;
using Testing::Scenario;

namespace {

std::string Narrow(const std::wstring& text) {
  std::string out;
  for (const auto c: text) {
    out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }
  return out;
}

void CheckGeometry(const GeometryOptions& options) {
  const auto geometry = BuildGeometry(options);
  const auto name = std::format("{}{}", options.mISO ? "ISO" : "ANSI", options.mFunctionRow ? " with F-keys" : "");
  Check(geometry.mWidth > 2.2f * geometry.mHeight, std::format("{}: much wider than tall ({:.2f} x {:.2f} units)", name,
    geometry.mWidth, geometry.mHeight));
  // Every key hit at its middle, and no two overlap
  bool hits = true;
  bool overlaps = false;
  for (std::size_t i = 0; i < geometry.mKeys.size(); ++i) {
    const auto [u, v] = KeyCentre(geometry, static_cast<int>(i));
    hits = hits && HitTest(geometry, u, v) == static_cast<int>(i);
    const auto& a = geometry.mKeys[i].mRect;
    for (std::size_t j = i + 1; j < geometry.mKeys.size(); ++j) {
      const auto& b = geometry.mKeys[j].mRect;
      const bool apart = a.mX + a.mWidth <= b.mX + 0.001f || b.mX + b.mWidth <= a.mX + 0.001f
        || a.mY + a.mHeight <= b.mY + 0.001f || b.mY + b.mHeight <= a.mY + 0.001f;
      overlaps = overlaps || !apart;
    }
  }
  Check(hits, name + ": every key is hit at its middle");
  Check(!overlaps, name + ": no two keys overlap");
  // The main block's rows are 15 units each
  const float left = geometry.mKeys.empty() ? 0 : geometry.mTopStrip.mX;
  for (const ScanCode first: {ScanCode {0x29}, ScanCode {0x0F}, ScanCode {0x3A}, ScanCode {0x2A}, ScanCode {0x1D}}) {
    const int key = FindKey(geometry, first);
    if (key < 0) {
      Check(false, std::format("{}: the row of scan {:#x} is there", name, first));
      continue;
    }
    const float y = geometry.mKeys[static_cast<std::size_t>(key)].mRect.mY;
    float width = 0;
    for (const auto& k: geometry.mKeys) {
      if (k.mRect.mY == y && k.mRect.mX < left + 15.0f) {
        width += k.mRect.mWidth;
      }
    }
    // ISO Enter's lower part sits on the home row
    if (options.mISO && first == 0x3A) {
      width += 1.25f;
    }
    Check(std::abs(width - 15.0f) < 0.01f, std::format("{}: the row of scan {:#x} is 15 units ({:.2f})", name, first, width));
  }
  for (const ScanCode left_out: {ScanCode {0xE05B}, ScanCode {0xE05C}, ScanCode {0xE05D}, ScanCode {0xE037}, ScanCode {0x46},
         ScanCode {0xE11D}}) {
    Check(FindKey(geometry, left_out) < 0, std::format("{}: no key {:#x} (Windows, Menu, Print Screen, Scroll Lock, Pause)",
      name, left_out));
  }
  Check(FindKey(geometry, 0x56) >= 0 == options.mISO, name + ": the key beside left Shift only on ISO");
  Check(FindKey(geometry, 0x3B) >= 0 == options.mFunctionRow, name + ": F1 only with the function row");
  Check(FindRole(geometry, KeyRole::Layout) >= 0 && FindRole(geometry, KeyRole::Hide) >= 0,
    name + ": the layout and hide keys are there");
  Check(HitTest(geometry, 0.002f, 0.5f) < 0, name + ": the margin hits nothing");
}

struct Typed {
  std::wstring mText;
  VirtualKeyStroke mLast;
  int mStrokes {0};
};

}// namespace

int main() {
  Scenario("Geometry: ANSI and ISO, with and without the function row (doc 07)", [] {
    CheckGeometry({.mISO = false, .mFunctionRow = true});
    CheckGeometry({.mISO = true, .mFunctionRow = true});
    CheckGeometry({.mISO = false, .mFunctionRow = false});
    CheckGeometry({.mISO = true, .mFunctionRow = false});
    const auto geometry = BuildGeometry({});
    Check(std::abs(geometry.mWidth * 24.0f / 1000.0f - 0.552f) < 0.001f, "24 mm keys make it 55 cm wide");
  });

  const auto us = KeyboardLayout::Layout::Load("00000409");
  const auto german = KeyboardLayout::Layout::Load("00000407");
  if (!Check(us && german, "the US and German layouts load")) {
    return Testing::Finish();
  }

  Scenario("US: letters, Shift latches for one key, a quick second tap locks, Caps Lock (doc 07)", [&] {
    Keyboard keyboard {{us}};
    const auto& geometry = *keyboard.GetGeometry();
    double now = 10;
    std::wstring text;
    const auto press = [&](ScanCode scan) {
      const int key = FindKey(geometry, scan);
      const auto result = keyboard.Press(key, now);
      keyboard.Release(key);
      now += 0.2;
      if (result.mStroke) {
        text += result.mStroke->mText;
      }
      return result;
    };
    const auto shift = [&] {
      const int key = FindKey(geometry, 0x2A);
      keyboard.Press(key, now);
      keyboard.Release(key);
    };
    press(0x23);
    Check(text == L"h", std::format("h types h ({})", Narrow(text)));
    shift();
    now += 0.1;
    Check(keyboard.GetModifier(Modifier::Shift) == ModifierState::Latched, "one tap latches Shift");
    Check(keyboard.GetLegend(FindKey(geometry, 0x23)).mText == L"H", "the legends show Shift while it's latched");
    const auto shifted = press(0x23);
    press(0x17);
    Check(text == L"hHi", std::format("latched Shift lasts one key ({})", Narrow(text)));
    Check(shifted.mStroke && shifted.mStroke->mShift, "the shifted key goes with Shift held");
    shift();
    now += 0.2;// within 400 ms
    shift();
    Check(keyboard.GetModifier(Modifier::Shift) == ModifierState::Locked, "a second tap within 400 ms locks it");
    press(0x23);
    press(0x17);
    Check(text == L"hHiHI", std::format("locked Shift lasts ({})", Narrow(text)));
    shift();
    Check(keyboard.GetModifier(Modifier::Shift) == ModifierState::Off, "tapping a locked Shift clears it");
    shift();
    now += 0.6;// too slow to lock
    shift();
    Check(keyboard.GetModifier(Modifier::Shift) == ModifierState::Off, "a second tap after 400 ms clears a latch");
    keyboard.Press(FindKey(geometry, 0x3A), now);
    Check(keyboard.CapsLock(), "Caps Lock toggles on");
    const auto capital = press(0x1E);
    Check(text.back() == L'A' && capital.mStroke->mCapsLock, "Caps Lock: a types A, with the lock in the stroke");
    keyboard.Press(FindKey(geometry, 0x3A), now);
    Check(!keyboard.CapsLock(), "and off again");
  });

  Scenario("US: Ctrl and Alt, Enter, Backspace, the numpad and Num Lock (doc 06, doc 07)", [&] {
    Keyboard keyboard {{us}};
    const auto& geometry = *keyboard.GetGeometry();
    double now = 10;
    const auto tap = [&](ScanCode scan) {
      const int key = FindKey(geometry, scan);
      auto result = keyboard.Press(key, now);
      keyboard.Release(key);
      now += 0.6;
      return result;
    };
    tap(0x1D);
    const auto copy = tap(0x2E);
    Check(copy.mStroke && copy.mStroke->mControl && copy.mStroke->mVirtualKey == 'C' && copy.mStroke->mText == L"\x03",
      "Ctrl+C: Ctrl held, its control character (as TranslateMessage)");
    tap(0x38);
    const auto altF4 = tap(0x3E);
    Check(altF4.mStroke && altF4.mStroke->mAlt && altF4.mStroke->mText.empty() && WindowKeyInjection::IsBlocked(*altF4.mStroke),
      "Alt+F4: Alt, no text, and the injection blocks it (INJ-8)");
    const auto enter = tap(0x1C);
    Check(enter.mEnter && enter.mStroke && enter.mStroke->mText == L"\r", "Enter types a carriage return");
    tap(0x1D);
    const auto controlEnter = tap(0x1C);
    Check(controlEnter.mStroke && controlEnter.mStroke->mText == L"\n", "Ctrl+Enter types a line feed");
    const auto padEnter = tap(0xE01C);
    Check(padEnter.mEnter, "the numpad's Enter is Enter too");
    const auto back = tap(0x0E);
    Check(back.mStroke && back.mStroke->mText == L"\b" && back.mStroke->mVirtualKey == VK_BACK, "Backspace");
    const auto seven = tap(0x47);
    Check(seven.mStroke && seven.mStroke->mText == L"7" && seven.mStroke->mVirtualKey == VK_NUMPAD7 && seven.mStroke->mNumLock,
      "Num Lock on: numpad 7 types 7");
    tap(0x45);
    const auto home = tap(0x47);
    Check(home.mStroke && home.mStroke->mText.empty() && home.mStroke->mVirtualKey == VK_HOME && !home.mStroke->mNumLock,
      "Num Lock off: numpad 7 is Home");
    const auto hideResult = keyboard.Press(FindRole(geometry, KeyRole::Hide), now);
    Check(hideResult.mHide && !hideResult.mStroke, "the hide key hides, and sends nothing");
  });

  Scenario("German: AltGr, dead keys, ISO, and the layout key (doc 06, K7, K8)", [&] {
    Keyboard keyboard {{german, us}};
    Check(keyboard.GetGeometry()->mOptions.mISO, "German is ISO: the tall Enter and the extra key");
    Check(keyboard.HasAltGr(), "German has AltGr");
    const auto& geometry = *keyboard.GetGeometry();
    double now = 10;
    std::wstring text;
    const auto tap = [&](int key) {
      auto result = keyboard.Press(key, now);
      keyboard.Release(key);
      now += 0.6;
      if (result.mStroke) {
        text += result.mStroke->mText;
      }
      return result;
    };
    tap(FindRole(geometry, KeyRole::AltGr));
    const auto at = tap(FindKey(geometry, 0x10));
    Check(at.mStroke && at.mStroke->mText == L"@" && at.mStroke->mControl && at.mStroke->mAlt,
      std::format("AltGr+Q types @ as Ctrl+Alt ({})", Narrow(at.mStroke ? at.mStroke->mText : L"")));
    Check(keyboard.GetLegend(FindKey(geometry, 0x10)).mSmall == L"@", "Q shows its AltGr @ in a corner");
    Check(keyboard.GetLegend(FindKey(geometry, 0x0D)).mDead, "´ is shown as a dead key");
    text.clear();
    const auto dead = tap(FindKey(geometry, 0x0D));
    Check(!dead.mStroke && keyboard.PendingDeadKey().has_value(), "´ alone sends nothing and waits");
    tap(FindKey(geometry, 0x12));
    Check(text == L"é" && !keyboard.PendingDeadKey(), "´ then e is é");
    tap(FindKey(geometry, 0x0D));
    tap(FindKey(geometry, 0x2D));// x doesn't compose
    Check(text == L"é´x", std::format("´ then x types both ({} characters)", text.size()));
    tap(FindKey(geometry, 0x1A));
    Check(text.back() == L'ü', "ü is a key of its own");
    const auto layout = keyboard.Press(FindRole(geometry, KeyRole::Layout), now);
    Check(layout.mLayoutChanged && keyboard.ActiveLayout() == us.get(), "the layout key goes to the next layout");
    Check(!keyboard.GetGeometry()->mOptions.mISO, "US is ANSI");
    keyboard.Press(FindRole(*keyboard.GetGeometry(), KeyRole::Layout), now);
    Check(keyboard.ActiveLayout() == german.get(), "and round again");
  });

  Scenario("Repeat: Windows' delay and rate, never faster than 50 ms, no burst, not Enter (K12, VK-3)", [&] {
    const auto fastest = RepeatTiming::FromWindows(0, 31);
    Check(fastest.mDelaySeconds == 0.25 && fastest.mIntervalSeconds >= RepeatTiming::MinimumInterval,
      std::format("the fastest Windows setting: 250 ms, then every {:.0f} ms (at least 50)", fastest.mIntervalSeconds * 1000));
    const auto slowest = RepeatTiming::FromWindows(3, 0);
    Check(slowest.mDelaySeconds == 1.0 && std::abs(slowest.mIntervalSeconds - 0.4) < 0.001, "the slowest: 1 s, then 2.5 a second");
    Check(RepeatTiming::FromWindows(99, -5).mDelaySeconds == 1.0, "odd settings are clamped");

    Keyboard keyboard {{us}};
    keyboard.SetRepeatTiming({0.5, 0.01});// asks for faster than the floor
    const auto& geometry = *keyboard.GetGeometry();
    const int a = FindKey(geometry, 0x1E);
    keyboard.Press(a, 100.0);
    Check(!keyboard.Repeat(100.4), "no repeat before the delay");
    const auto first = keyboard.Repeat(100.5);
    Check(first && first->mRepeat && first->mText == L"a", "the first repeat at the delay, marked as a repeat");
    Check(!keyboard.Repeat(100.53), "the next not sooner than 50 ms (VK-3)");
    Check(keyboard.Repeat(100.56).has_value(), "the next at 50 ms");
    // A stall: one repeat, then the rate again from now
    Check(keyboard.Repeat(102.0).has_value() && !keyboard.Repeat(102.0) && !keyboard.Repeat(102.03),
      "after a stall: one repeat, never a burst");
    keyboard.Release(a);
    Check(!keyboard.Repeat(103.0), "let go: no more");
    const int enter = FindKey(geometry, 0x1C);
    keyboard.Press(enter, 200.0);
    Check(!keyboard.Repeat(205.0), "Enter never repeats (it would send messages in chat apps)");
    keyboard.Release(enter);
    const int shift = FindKey(geometry, 0x2A);
    keyboard.Press(shift, 300.0);
    keyboard.Press(a, 300.1);
    const auto repeat = keyboard.Repeat(300.7);
    Check(repeat && repeat->mText == L"A" && repeat->mShift, "a key typed with a latched Shift repeats with it");
  });

  Scenario("Auto-show: a caret near the press within 1.5 s shows it; anything else doesn't (VK-1, K3)", [] {
    const HWND window = reinterpret_cast<HWND>(0x1234);
    const HWND other = reinterpret_cast<HWND>(0x5678);
    const POINT tap {500, 400};
    const RECT nearCaret {480, 390, 482, 410};
    const RECT farCaret {100, 900, 102, 920};
    Check(AutoShow::TapChecks[0].count() == 80 && AutoShow::TapChecks[1].count() == 400 && AutoShow::EnterCheck.count() == 150,
      "re-checks 80 and 400 ms after a press, 150 ms after Enter (VK-2)");
    {
      AutoShow show;
      show.Tapped(10.0, window, tap);
      const auto change = show.Caret(10.2, window, true, nearCaret);
      Check(change && change->mShown && show.IsShown(), "a caret level with the press, 200 ms after: shown");
    }
    {
      AutoShow show;
      show.Tapped(10.0, window, tap);
      Check(!show.Caret(10.2, window, true, farCaret) && !show.IsShown(),
        "the app focusing a field far from the press (Discord on a channel switch): not shown");
    }
    {
      AutoShow show;
      show.Tapped(10.0, window, tap);
      Check(!show.Caret(11.6, window, true, nearCaret), "a caret near it 1.6 s later: not shown");
    }
    {
      AutoShow show;
      Check(!show.Caret(10.2, window, true, nearCaret), "a caret with no press at all: not shown");
      show.Tapped(10.0, window, tap);
      Check(!show.Caret(10.1, other, true, nearCaret), "a report from a window no longer watched is ignored");
    }
    Check(AutoShow::IsNear({0, 100, 2, 112}, {0, 232}) && !AutoShow::IsNear({0, 100, 2, 112}, {0, 233}),
      "near: 120 px below a small caret, not 121");
    Check(AutoShow::IsNear({0, 100, 2, 150}, {0, 350}) && !AutoShow::IsNear({0, 100, 2, 150}, {0, 351}),
      "near: 4 caret heights below a tall caret (200 px), not more");
  });

  Scenario("Auto-hide, Enter, the keybind and dismissing (K4, K5, K6)", [] {
    const HWND window = reinterpret_cast<HWND>(0x1234);
    const POINT tap {500, 400};
    const RECT caret {480, 390, 482, 410};
    AutoShow show;
    show.Tapped(10.0, window, tap);
    show.Caret(10.1, window, true, caret);
    Check(!show.Caret(10.2, window, false, std::nullopt), "focus leaving within 350 ms of a press: not yet (the 400 ms check decides)");
    const auto hidden = show.Caret(10.45, window, false, std::nullopt);
    Check(hidden && !hidden->mShown && !show.IsShown(), "focus left the window: hidden");
    // Enter, without and with "Hide after Enter"
    show.Tapped(20.0, window, tap);
    show.Caret(20.1, window, true, caret);
    Check(!show.Enter(21.0) && show.IsShown(), "Enter keeps it by default (Shift+Enter, send and keep typing)");
    show.SetHideAfterEnter(true);
    const auto afterEnter = show.Enter(22.0);
    Check(afterEnter && !afterEnter->mShown, "with \"Hide after Enter\", Enter hides it");
    // The keybind: shown that way, it stays until dismissed
    const auto toggled = show.Toggle();
    Check(toggled.mShown && show.GetMode() == AutoShow::Mode::Manual, "the keybind shows it");
    Check(!show.Caret(40.0, window, false, std::nullopt) && show.IsShown(), "shown by the keybind, no caret doesn't hide it");
    Check(!show.Toggle().mShown && !show.IsShown(), "the keybind hides it again");
    show.Toggle();
    const auto dismissed = show.Dismiss("all panels were hidden");
    Check(dismissed && !dismissed->mShown && !show.IsShown(), "dismissing hides it");
    Check(!show.Dismiss("again"), "dismissing it hidden changes nothing");
  });

  Scenario("The shared model: hover counts only when it changes; a press is urgent; changes wake the renderer", [] {
    KeyboardModel model;
    int wakes = 0;
    model.SetWake([&] { ++wakes; });
    model.SetHover({3, -1}, {2, 0});
    model.SetHover({3, -1}, {2, 0});
    Check(wakes == 1, std::format("the same hover twice wakes once ({})", wakes));
    Check(!model.TakeUrgent(), "hover isn't urgent");
    model.Flash(5);
    Check(model.TakeUrgent() && !model.TakeUrgent(), "a key press is urgent, once");
    const auto frame = model.Frame();
    Check(frame.mFlashKey[0] == 5 || frame.mFlashKey[1] == 5 || frame.mFlashKey[2] == 5 || frame.mFlashKey[3] == 5,
      "the pressed key flashes");
    model.SetShown(true);
    Check(model.IsShown() && model.TakeUrgent(), "showing it is drawn at once");
    model.SetTyping(true);
    const int before = wakes;
    model.SetTyping(false);
    Check(wakes == before, "typing or not changes nothing drawn while both opacities match");
    model.SetWake({});
    model.Flash(6);
    Check(wakes == before, "no renderer: no wake");
  });

  return Testing::Finish();
}
