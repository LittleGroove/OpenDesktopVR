// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Keybinds (doc 09):
// - when a binding fires (KY-1): on a release with the others held; a
//   modifier alone never; per device; the most specific binding; held
//   buttons forgotten when devices change; a lost device's releases fire
//   nothing; switches and detents left on don't block (test round 7);
// - bind capture (KY-4): Escape cancels; buttons held at the start are
//   ignored; a modifier alone starts over;
// - re-acquire back-off (KY-2): 100 ms doubling to 5 s, reset only by a
//   successful read;
// - the stored form, and names;
// - the real input thread: keys sent with SendInput (F24, unused by
//   anything) fire a binding through Raw Input, and a capture takes them.

#include "check.hpp"

#include <OpenDesktopVR/BindCapture.hpp>
#include <OpenDesktopVR/ChordTracker.hpp>
#include <OpenDesktopVR/InputThread.hpp>
#include <OpenDesktopVR/Keybinds.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <Windows.h>

#include <atomic>
#include <format>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Keybinds;
using namespace std::chrono_literals;
using Testing::Check;
using Testing::Scenario;

namespace {

Button Key(UINT vk) {
  return Button {DeviceKind::Keyboard, {}, vk};
}

Button Pad(const std::string& device, uint32_t code) {
  return Button {DeviceKind::Controller, device, code, "{product}", "Stick"};
}

const Action ToggleAll {ActionKind::ToggleAllPanels};
const Action TogglePanel1 {ActionKind::TogglePanel, 1};
const Action Brighter1 {ActionKind::BrightnessUp, 1};

// Presses then releases, in order; the actions that fired
std::vector<Action> Run(ChordTracker& tracker, const std::vector<std::pair<Button, bool>>& events) {
  std::vector<Action> fired;
  for (const auto& [button, pressed]: events) {
    if (auto action = tracker.OnButton(button, pressed)) {
      fired.push_back(*action);
    }
  }
  return fired;
}

// The same, each event at a time (milliseconds from a fixed start)
struct Timed {
  Button mButton;
  bool mPressed;
  int mAtMs;
};
const TimeUtil::TimePoint Start = TimeUtil::Now();
std::vector<Action> RunAt(ChordTracker& tracker, const std::vector<Timed>& events) {
  std::vector<Action> fired;
  for (const auto& event: events) {
    if (auto action = tracker.OnButton(event.mButton, event.mPressed, Start + std::chrono::milliseconds {event.mAtMs})) {
      fired.push_back(*action);
    }
  }
  return fired;
}

void SendKey(WORD vk, bool up) {
  INPUT input {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
  SendInput(1, &input, sizeof(input));
}

bool WaitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
  const auto end = TimeUtil::Now() + limit;
  while (TimeUtil::Now() < end) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return condition();
}

}// namespace

int main() {
  Scenario("A key fires on release; Ctrl+key fires as a combination, not as the key (KY-1)", [] {
    ChordTracker tracker;
    tracker.SetBindings({
      {Binding::From({Key(VK_F13)}), ToggleAll},
      {Binding::From({Key(VK_CONTROL), Key(VK_F13)}), TogglePanel1},
    });
    auto fired = Run(tracker, {{Key(VK_F13), true}});
    Check(fired.empty(), "nothing on the press");
    fired = Run(tracker, {{Key(VK_F13), false}});
    Check(fired.size() == 1 && fired[0] == ToggleAll, "F13 fires when let go");
    fired = Run(tracker, {{Key(VK_CONTROL), true}, {Key(VK_F13), true}, {Key(VK_F13), false}, {Key(VK_CONTROL), false}});
    Check(fired.size() == 1 && fired[0] == TogglePanel1, "Ctrl+F13 fires once, as itself, not as F13");
    fired = Run(tracker, {{Key(VK_CONTROL), true}, {Key(VK_F13), true}, {Key(VK_CONTROL), false}, {Key(VK_F13), false}});
    Check(fired.size() == 1 && fired[0] == TogglePanel1, "letting go of Ctrl first fires it too, once");
  });

  Scenario("A modifier on its own never fires, even if bound; it's not a valid binding", [] {
    ChordTracker tracker;
    const auto ctrl = Binding::From({Key(VK_CONTROL)});
    Check(!ctrl.IsValid(), "Ctrl alone isn't a binding");
    tracker.SetBindings({{ctrl, ToggleAll}});
    Check(Run(tracker, {{Key(VK_CONTROL), true}, {Key(VK_CONTROL), false}}).empty(), "Ctrl pressed and let go: nothing");
  });

  Scenario("A key held for the game doesn't block a binding; another modifier does", [] {
    ChordTracker tracker;
    tracker.SetBindings({{Binding::From({Key(VK_F13)}), ToggleAll}});
    auto fired = Run(tracker, {{Key('W'), true}, {Key(VK_F13), true}, {Key(VK_F13), false}, {Key('W'), false}});
    Check(fired.size() == 1, "W held (flying), F13 pressed: fires");
    fired = Run(tracker, {{Key(VK_SHIFT), true}, {Key(VK_F13), true}, {Key(VK_F13), false}, {Key(VK_SHIFT), false}});
    Check(fired.empty(), "Shift+F13 (maybe the game's): F13 doesn't fire");
  });

  Scenario("A HOTAS shift button: shift+B1, then shift+B2 while still holding shift", [] {
    ChordTracker tracker;
    tracker.SetBindings({
      {Binding::From({Pad("{a}", 0), Pad("{a}", 1)}), ToggleAll},
      {Binding::From({Pad("{a}", 0), Pad("{a}", 2)}), TogglePanel1},
      {Binding::From({Pad("{a}", 0)}), Brighter1},
    });
    const auto fired = Run(tracker, {{Pad("{a}", 0), true}, {Pad("{a}", 1), true}, {Pad("{a}", 1), false},
      {Pad("{a}", 2), true}, {Pad("{a}", 2), false}, {Pad("{a}", 0), false}});
    Check(fired.size() == 2 && fired[0] == ToggleAll && fired[1] == TogglePanel1, "both combinations fired, in order");
    Check(std::ranges::find(fired, Brighter1) == fired.end(), "the shift button held through them doesn't fire alone");
    const auto alone = Run(tracker, {{Pad("{a}", 0), true}, {Pad("{a}", 0), false}});
    Check(alone.size() == 1 && alone[0] == Brighter1, "pressed alone, it does");
  });

  Scenario("State is per device; a button on another controller doesn't spoil a combination", [] {
    ChordTracker tracker;
    tracker.SetBindings({{Binding::From({Pad("{a}", 3)}), ToggleAll}});
    const auto fired = Run(tracker, {{Pad("{b}", 0), true}, {Pad("{a}", 3), true}, {Pad("{a}", 3), false}});
    Check(fired.size() == 1, "the throttle's button held, the stick's button fires");
    Check(Run(tracker, {{Pad("{b}", 3), true}, {Pad("{b}", 3), false}}).empty(), "the same button on another device isn't bound");
  });

  Scenario("Devices changed: held buttons are forgotten (KY-1)", [] {
    ChordTracker tracker;
    tracker.SetBindings({{Binding::From({Key(VK_F13)}), ToggleAll}});
    Run(tracker, {{Key(VK_F13), true}});
    tracker.Clear();
    Check(Run(tracker, {{Key(VK_F13), false}}).empty(), "a release of something held before the change fires nothing");
  });

  Scenario("A lost device back: what was let go meanwhile fires nothing (KY-2)", [] {
    ChordTracker tracker;
    tracker.SetBindings({{Binding::From({Pad("{a}", 0), Pad("{a}", 1)}), ToggleAll}});
    Run(tracker, {{Pad("{a}", 0), true}, {Pad("{a}", 1), true}});
    tracker.Resync(DeviceKind::Controller, "{a}", {0});// 1 was let go while it was lost
    Check(Run(tracker, {{Pad("{a}", 0), false}}).empty(), "nothing fires for the rest of that combination");
    const auto again = Run(tracker, {{Pad("{a}", 0), true}, {Pad("{a}", 1), true}, {Pad("{a}", 1), false}});
    Check(again.size() == 1, "pressed again, it works");
  });

  Scenario("A switch or detent on from the start doesn't block a controller binding (test round 7)", [] {
    ChordTracker tracker;
    tracker.SetBindings({{Binding::From({Pad("{a}", 56)}), ToggleAll}});
    tracker.Resync(DeviceKind::Controller, "{a}", {12, 30});// a toggle switch on, the throttle in a detent
    auto fired = Run(tracker, {{Pad("{a}", 56), true}, {Pad("{a}", 56), false}});
    Check(fired.size() == 1 && fired[0] == ToggleAll, "button 57 fires with two buttons held from the start");
    fired = Run(tracker, {{Pad("{a}", 56), true}, {Pad("{a}", 56), false}});
    Check(fired.size() == 1, "and again");
    Check(!tracker.TakeMiss(), "no miss reported");
    Check(Run(tracker, {{Pad("{a}", 12), false}}).empty(), "the switch turned off fires nothing");
  });

  Scenario("A switch flipped on in flight stops blocking after a second; a quick shift press still does", [] {
    ChordTracker tracker;
    tracker.SetBindings({{Binding::From({Pad("{a}", 5)}), ToggleAll}});
    auto fired = RunAt(tracker, {{Pad("{a}", 30), true, 0}, {Pad("{a}", 5), true, 2000}, {Pad("{a}", 5), false, 2100}});
    Check(fired.size() == 1, "switch on 2 s before: button 6 fires");
    fired = RunAt(tracker, {{Pad("{a}", 5), true, 2500}, {Pad("{a}", 5), false, 2600}});
    Check(fired.size() == 1, "and keeps firing");
    // A quick press of another button (the game's shift) is part of the combination
    fired = RunAt(tracker, {{Pad("{a}", 7), true, 3000}, {Pad("{a}", 5), true, 3300}, {Pad("{a}", 5), false, 3400}});
    Check(fired.empty(), "button 8 pressed 0.3 s before: that's the game's shift + button 6, not button 6");
    const auto miss = tracker.TakeMiss();
    Check(miss && miss->mAlsoPressed.size() == 1 && miss->mAlsoPressed[0].mCode == 7,
      "the log can say why: button 8 was pressed with it (not the switch)");
    Check(!tracker.TakeMiss(), "reported once");
    RunAt(tracker, {{Pad("{a}", 7), false, 3500}});
    fired = RunAt(tracker, {{Pad("{a}", 5), true, 3600}, {Pad("{a}", 5), false, 3700}});
    Check(fired.size() == 1, "shift let go: button 6 fires again");
  });

  Scenario("A shift held a while, then B1: still shift+B1; a binding can include a switch", [] {
    ChordTracker tracker;
    tracker.SetBindings({
      {Binding::From({Pad("{a}", 0), Pad("{a}", 1)}), ToggleAll},
      {Binding::From({Pad("{a}", 1)}), Brighter1},
    });
    auto fired = RunAt(tracker, {{Pad("{a}", 0), true, 0}, {Pad("{a}", 1), true, 2000}, {Pad("{a}", 1), false, 2100}});
    Check(fired.size() == 1 && fired[0] == ToggleAll, "the most specific binding wins, however long shift was held");
    fired = RunAt(tracker, {{Pad("{a}", 0), false, 2200}, {Pad("{a}", 1), true, 2300}, {Pad("{a}", 1), false, 2400}});
    Check(fired.size() == 1 && fired[0] == Brighter1, "shift let go: B1 alone");
  });

  Scenario("Keyboard: a key held for the game, Ctrl pressed and let go, then F13: fires (it didn't, while W was held)", [] {
    ChordTracker tracker;
    tracker.SetBindings({
      {Binding::From({Key(VK_F13)}), ToggleAll},
      {Binding::From({Key(VK_CONTROL), Key(VK_F13)}), TogglePanel1},
    });
    auto fired = RunAt(tracker, {{Key('W'), true, 0}, {Key(VK_CONTROL), true, 1500}, {Key(VK_CONTROL), false, 1600},
      {Key(VK_F13), true, 2000}, {Key(VK_F13), false, 2100}});
    Check(fired.size() == 1 && fired[0] == ToggleAll, "F13 fires as itself");
    // Ctrl held two seconds, then F13: that's Ctrl+F13, even let go in either order
    fired = RunAt(tracker, {{Key(VK_CONTROL), true, 3000}, {Key(VK_F13), true, 5000}, {Key(VK_F13), false, 5100},
      {Key(VK_CONTROL), false, 5200}});
    Check(fired.size() == 1 && fired[0] == TogglePanel1, "Ctrl held long, then F13: Ctrl+F13");
    fired = RunAt(tracker, {{Key(VK_CONTROL), true, 6000}, {Key(VK_F13), true, 8000}, {Key(VK_CONTROL), false, 8100},
      {Key(VK_F13), false, 8200}});
    Check(fired.size() == 1 && fired[0] == TogglePanel1, "Ctrl let go first: Ctrl+F13 once, never F13");
    Check(!tracker.TakeMiss(), "keyboard keys are never reported");
  });

  Scenario("Keys missed on the lock screen are dropped", [] {
    ChordTracker tracker;
    tracker.SetBindings({{Binding::From({Key(VK_F13)}), ToggleAll}});
    Run(tracker, {{Key(VK_CONTROL), true}});// let go on the lock screen: never seen
    tracker.DropKeys([](uint32_t) { return true; });
    Check(Run(tracker, {{Key(VK_F13), true}, {Key(VK_F13), false}}).size() == 1, "F13 isn't taken for Ctrl+F13");
  });

  Scenario("Bind capture: a combination, Escape, held buttons, a modifier alone (KY-4)", [] {
    BindCapture capture;
    capture.Ignore({Key(VK_F14)});
    capture.OnButton(Key(VK_F14), false);// held before: ignored
    Check(capture.GetState() == BindCapture::State::Waiting, "the release of a key held at the start is ignored");
    capture.OnButton(Key(VK_CONTROL), true);
    capture.OnButton(Key(VK_CONTROL), false);
    Check(capture.GetState() == BindCapture::State::Waiting && capture.ModifierOnly(), "Ctrl alone: starts over, and says why");
    capture.OnButton(Key(VK_CONTROL), true);
    capture.OnButton(Key(VK_F13), true);
    Check(capture.GetState() == BindCapture::State::Holding, "holding Ctrl+F13");
    capture.OnButton(Key(VK_F13), false);
    Check(capture.GetState() == BindCapture::State::Holding, "not done until everything's let go");
    capture.OnButton(Key(VK_CONTROL), false);
    Check(capture.GetState() == BindCapture::State::Done, "done");
    Check(capture.Current() == Binding::From({Key(VK_CONTROL), Key(VK_F13)}), "the binding is Ctrl+F13");
    Check(BindingName(capture.Current()) == "Ctrl + F13", std::format("named '{}'", BindingName(capture.Current())));

    BindCapture escape;
    escape.OnButton(Key(VK_ESCAPE), true);
    Check(escape.GetState() == BindCapture::State::Cancelled, "Escape cancels; it can't be bound");
  });

  Scenario("Re-acquire back-off: 100 ms doubling to 5 s; only a read resets it (KY-2)", [] {
    Backoff backoff;
    std::vector<int64_t> delays;
    for (int i = 0; i < 8; ++i) {
      delays.push_back(backoff.Next().count());
    }
    Check(delays == std::vector<int64_t> {100, 200, 400, 800, 1600, 3200, 5000, 5000}, "100, 200, ... 5000, 5000");
    backoff.Succeeded();
    Check(backoff.Next().count() == 100, "a successful read starts it over");
  });

  Scenario("Stored as readable JSON; bad entries read as empty (UI-5, TH-9)", [] {
    Slots slots;
    slots[0] = Binding::From({Key(VK_CONTROL), Key(VK_F13)});
    slots[1] = Binding::From({Pad("{1234}", HatCode(0, HatDirection::Up)), Pad("{1234}", 4)});
    const auto json = ToJson(slots);
    const auto back = SlotsFromJson(json);
    Check(back[0] == slots[0] && back[1] == slots[1], "both slots round-trip");
    Check(json[0][1].value("key", "") == "F13" || json[0][0].value("key", "") == "F13", "keys are written with their names");
    Check(BindingName(back[1]) == "Stick: button 5 + Stick: hat 1 up", std::format("controller names: '{}'", BindingName(back[1])));
    Check(SlotsFromJson(JsonUtil::Json::parse(R"([[{"vk": 999}], [{"device": ""}]])"))[0].IsEmpty(), "a bad key: empty");
    Check(SlotsFromJson(JsonUtil::Json::parse(R"([[{"vk": 17}], 5])"))[0].IsEmpty(), "a modifier alone, hand-edited: empty");
    Check(SlotsFromJson(JsonUtil::Json::parse(R"("nonsense")"))[1].IsEmpty(), "not a list: empty, no exception");
  });

  Scenario("The real input thread: Raw Input sees keys sent while another window has focus", [] {
    std::atomic<int> fired {0};
    InputThread input {[&](const Action&) { ++fired; }};
    Check(WaitFor([&] { return input.KeyboardReady(); }, 2s), "the keyboard is watched (Raw Input, RIDEV_INPUTSINK)");
    input.SetBindings({{Binding::From({Key(VK_F24)}), ToggleAll}});
    std::this_thread::sleep_for(100ms);
    SendKey(VK_F24, false);
    SendKey(VK_F24, true);
    const bool firedOnce = WaitFor([&] { return fired.load() == 1; }, 2s);
    Check(firedOnce, std::format("F24 fired the binding ({} times)", fired.load()));

    const auto capture = input.StartCapture();
    SendKey(VK_SHIFT, false);
    SendKey(VK_F24, false);
    SendKey(VK_F24, true);
    SendKey(VK_SHIFT, true);
    Check(WaitFor([&] { return capture->State() == BindCapture::State::Done; }, 2s), "a capture took Shift+F24");
    Check(capture->Current() == Binding::From({Key(VK_SHIFT), Key(VK_F24)}), std::format("as '{}'", BindingName(capture->Current())));
    Check(fired.load() == 1, "and no binding fired while it was capturing");
    std::printf("  (game controllers here: %zu)\n", input.Controllers().size());
  });

  return Testing::Finish();
}
