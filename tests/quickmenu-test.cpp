// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The quick menu (M6d, ADR-041), without a headset:
// - every page, with few and many panels, windows and profiles: every
//   target at least 20 mm each way in VR (DU-6), inside the panel, not
//   overlapping another, and hit at its middle;
// - where commands take it: pages, back, lists' rows, scrolling;
// - touches: a tap acts when the finger lifts off the target it pressed,
//   not after sliding off, not from a press that started elsewhere, not
//   when tracking is lost, and still on its own target after the layout
//   was rebuilt mid-press (DU-7); two-finger scroll in rows;
// - the shared model: only picture changes wake the renderer (RN-6), and
//   a stale layout's pointer is ignored;
// - the hand thread's queue: taps in order, scrolls merged, the UI thread
//   told once a command, a full queue dropping and counting (TH-5);
// - "quickMenu" in settings.json: defaults, limits, a round trip;
// - its panel: placed low on the right, sized by its pixels, given to the
//   renderer after the keyboard and before the panels.

#include "check.hpp"

#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/QuickMenu.hpp>
#include <OpenDesktopVR/QuickMenuConfig.hpp>
#include <OpenDesktopVR/QuickMenuHands.hpp>
#include <OpenDesktopVR/QuickMenuModel.hpp>

#include <cmath>
#include <format>
#include <limits>
#include <string>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::QuickMenu;
using Testing::Check;
using Testing::Scenario;

namespace {

MenuData MakeData(std::size_t panels, std::size_t profiles, std::optional<std::size_t> windows) {
  MenuData data;
  for (std::size_t i = 0; i < panels; ++i) {
    PanelInfo panel;
    panel.mID = 100 + i;
    panel.mName = i == 1 ? L"A panel with a very long name that can't possibly fit on one row of the menu"
                         : std::format(L"Panel {}", i + 1);
    panel.mWindow = i % 2 == 0;
    panel.mShowing = i != 2;
    data.mPanels.push_back(panel);
  }
  for (std::size_t i = 0; i < profiles; ++i) {
    data.mProfiles.push_back({std::format("p{}", i), std::format(L"Profile {}", i + 1), i == 0});
  }
  data.mProfileName = profiles > 0 ? data.mProfiles[0].mName : L"";
  data.mAnyShowing = panels > 0;
  if (windows) {
    std::vector<WindowEntry> list;
    for (std::size_t i = 0; i < *windows; ++i) {
      list.push_back({std::format(L"Window {} - a title long enough to be trimmed at the end", i + 1), L"app"});
    }
    data.mWindows = std::move(list);
  }
  return data;
}

int FindTarget(const Layout& layout, CommandKind kind, uint64_t arg = 0, int step = 0) {
  for (std::size_t i = 0; i < layout.mItems.size(); ++i) {
    const auto& command = layout.mItems[i].mCommand;
    if (command.mKind == kind && command.mArg == arg && command.mStep == step && layout.mItems[i].IsTarget()) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// (u, v) of an item's middle
std::pair<float, float> Middle(const Layout& layout, int index) {
  const auto& rect = layout.mItems[static_cast<std::size_t>(index)].mRect;
  return {((rect.mLeft + rect.mRight) / 2) / static_cast<float>(layout.mWidth),
    ((rect.mTop + rect.mBottom) / 2) / static_cast<float>(layout.mHeight)};
}

void CheckLayout(const std::string& name, const Layout& layout) {
  const float minPixels = MinTargetMillimeters * PixelsPerMillimeter;
  bool sizes = true;
  bool inside = true;
  bool apart = true;
  bool hit = true;
  int targets = 0;
  std::string problem;
  for (std::size_t i = 0; i < layout.mItems.size(); ++i) {
    const auto& item = layout.mItems[i];
    const auto& r = item.mRect;
    if (r.mLeft < 0 || r.mTop < 0 || r.mRight > static_cast<float>(layout.mWidth) + 0.01f
        || r.mBottom > static_cast<float>(layout.mHeight) + 0.01f || r.Width() <= 0 || r.Height() <= 0) {
      inside = false;
      problem = std::format("item {} outside", i);
    }
    if (item.mCommand.mKind == CommandKind::None) {
      continue;
    }
    ++targets;
    if (r.Width() < minPixels || r.Height() < minPixels) {
      sizes = false;
      problem = std::format("target {} is {:.1f} x {:.1f} mm", i, r.Width() / PixelsPerMillimeter, r.Height() / PixelsPerMillimeter);
    }
    for (std::size_t j = i + 1; j < layout.mItems.size(); ++j) {
      const auto& other = layout.mItems[j];
      if (other.mCommand.mKind == CommandKind::None) {
        continue;
      }
      const auto& o = other.mRect;
      if (r.mLeft < o.mRight && o.mLeft < r.mRight && r.mTop < o.mBottom && o.mTop < r.mBottom) {
        apart = false;
        problem = std::format("targets {} and {} overlap", i, j);
      }
    }
    if (item.IsTarget()) {
      const float x = (r.mLeft + r.mRight) / 2;
      const float y = (r.mTop + r.mBottom) / 2;
      if (HitTest(layout, x, y) != static_cast<int>(i)) {
        hit = false;
        problem = std::format("target {} not hit at its middle", i);
      }
    }
  }
  Check(sizes && inside && apart && hit && targets > 0,
    std::format("{}: {} targets, each at least 20 mm each way, inside, apart, hit at the middle ({} x {} mm){}", name, targets,
      std::lround(layout.mWidth / PixelsPerMillimeter), std::lround(layout.mHeight / PixelsPerMillimeter),
      problem.empty() ? "" : "; " + problem));
}

}// namespace

int main() {
  Scenario("Every page: targets at least 20 mm each way (DU-6), inside, apart, hit at the middle", [] {
    CheckLayout("closed", BuildLayout({}, MakeData(3, 1, std::nullopt)));
    for (const std::size_t panels: {0u, 3u, 5u, 7u}) {
      const auto data = MakeData(panels, 2, std::nullopt);
      CheckLayout(std::format("home, {} panels", panels), BuildLayout({.mOpen = true}, data));
      if (panels > 5) {
        CheckLayout(std::format("home, {} panels, second rows", panels), BuildLayout({.mOpen = true, .mFirst = 5}, data));
      }
    }
    const auto data = MakeData(4, 8, 12);
    CheckLayout("panel page (a window)", BuildLayout({.mOpen = true, .mPage = Page::Panel, .mPanel = 100}, data));
    CheckLayout("panel page (not a window)", BuildLayout({.mOpen = true, .mPage = Page::Panel, .mPanel = 101}, data));
    CheckLayout("panel page (gone)", BuildLayout({.mOpen = true, .mPage = Page::Panel, .mPanel = 999}, data));
    CheckLayout("windows, 12", BuildLayout({.mOpen = true, .mPage = Page::Windows, .mPanel = 100}, data));
    CheckLayout("windows, finding", BuildLayout({.mOpen = true, .mPage = Page::Windows}, MakeData(1, 1, std::nullopt)));
    CheckLayout("windows, none", BuildLayout({.mOpen = true, .mPage = Page::Windows}, MakeData(1, 1, 0)));
    CheckLayout("profiles, 8", BuildLayout({.mOpen = true, .mPage = Page::Profiles}, data));
    CheckLayout("profiles, 1", BuildLayout({.mOpen = true, .mPage = Page::Profiles}, MakeData(1, 1, std::nullopt)));
    auto settings = data;
    CheckLayout("settings", BuildLayout({.mOpen = true, .mPage = Page::Settings}, settings));
    settings.mButton = false;
    CheckLayout("settings, button off", BuildLayout({.mOpen = true, .mPage = Page::Settings}, settings));

    const auto closed = BuildLayout({}, data);
    Check(closed.mWidth == ButtonSize && closed.mHeight == ButtonSize && closed.mItems.size() == 1
        && closed.mItems[0].mCommand.mKind == CommandKind::Open,
      "closed: one 40 mm button that opens it");
    const auto home = BuildLayout({.mOpen = true}, MakeData(5, 1, std::nullopt));
    Check(home.mWidth == Width && home.mHeight / PixelsPerMillimeter < 240,
      std::format("home with 5 panels: 256 mm wide, {:.0f} mm tall (under 240)", home.mHeight / PixelsPerMillimeter));
    Check(FindTarget(home, CommandKind::TogglePanel, 102) >= 0 && !home.mItems[static_cast<std::size_t>(FindTarget(home, CommandKind::TogglePanel, 102))].mOn,
      "a hidden panel's switch is off");
    const auto many = BuildLayout({.mOpen = true}, MakeData(7, 1, std::nullopt));
    Check(FindTarget(many, CommandKind::NextRows) >= 0 && FindTarget(many, CommandKind::PreviousRows) < 0,
      "7 panels: Next, and Previous greyed out at the top");
    const auto nothing = BuildLayout({.mOpen = true}, MakeData(0, 1, std::nullopt));
    Check(FindTarget(nothing, CommandKind::ToggleAll) < 0 && FindTarget(nothing, CommandKind::Recenter) >= 0,
      "no panels: Show all greyed out, Recenter still there");
    auto top = MakeData(4, 1, std::nullopt);
    top.mPanels[0].mOpacity = 1.0f;
    top.mPanels[0].mBrightness = MinLevel;
    const auto limits = BuildLayout({.mOpen = true, .mPage = Page::Panel, .mPanel = 100}, top);
    Check(FindTarget(limits, CommandKind::Opacity, 100, 1) < 0 && FindTarget(limits, CommandKind::Opacity, 100, -1) >= 0
        && FindTarget(limits, CommandKind::Brightness, 100, -1) < 0,
      "steppers at their limits are greyed out (opacity 100%: no +; brightness 10%: no -)");
    Check(FindTarget(limits, CommandKind::ShowWindows, 100) >= 0
        && FindTarget(BuildLayout({.mOpen = true, .mPage = Page::Panel, .mPanel = 101}, top), CommandKind::ShowWindows, 101) < 0,
      "Change window only for a window panel");
  });

  Scenario("Where commands take the menu", [] {
    const auto data = MakeData(7, 2, 12);
    MenuState state;
    state = Navigate(state, {CommandKind::Open}, data);
    Check(state.mOpen && state.mPage == Page::Home, "Open: the start page");
    state = Navigate(state, {CommandKind::NextRows}, data);
    Check(state.mFirst == 2, std::format("7 panels, Next: rows 3 to 7 (first {})", state.mFirst));
    state = Navigate(state, {CommandKind::ShowPanelPage, 103}, data);
    Check(state.mPage == Page::Panel && state.mPanel == 103 && state.mFirst == 0, "a panel's row: its page");
    state = Navigate(state, {CommandKind::Opacity, 103, 1}, data);
    Check(state.mPage == Page::Panel, "a host command stays on the page");
    state = Navigate(state, {CommandKind::ShowWindows, 103}, data);
    Check(state.mPage == Page::Windows && state.mPanel == 103, "Change window: the windows, for that panel");
    state = Navigate(state, {CommandKind::NextRows}, data);
    state = Navigate(state, {CommandKind::NextRows}, data);
    state = Navigate(state, {CommandKind::NextRows}, data);
    Check(state.mFirst == 7, std::format("12 windows: Next stops at the last five (first {})", state.mFirst));
    state = Navigate(state, {CommandKind::PreviousRows}, data);
    Check(state.mFirst == 2, "Previous: five back");
    state = Navigate(state, {CommandKind::Scroll, 0, -10}, data);
    Check(state.mFirst == 0, "scrolling up past the top stops there");
    state = Navigate(state, {CommandKind::Scroll, 0, 3}, data);
    Check(state.mFirst == 3, "scrolling down 3 rows");
    state = Navigate(state, {CommandKind::PickWindow, 4}, data);
    Check(state.mPage == Page::Panel && state.mPanel == 103, "picking a window: back to its panel");
    state = Navigate(state, {CommandKind::ShowWindows, 103}, data);
    state = Navigate(state, {CommandKind::Back}, data);
    Check(state.mPage == Page::Panel, "Back from the windows: the panel");
    state = Navigate(state, {CommandKind::Back}, data);
    Check(state.mPage == Page::Home, "Back from a panel: the start");
    state = Navigate(state, {CommandKind::ShowProfiles}, data);
    state = Navigate(state, {CommandKind::SwitchProfile, 1}, data);
    Check(state.mPage == Page::Home, "switching profile: back to the start");
    state = Navigate(state, {CommandKind::ShowSettings}, data);
    state = Navigate(state, {CommandKind::Close}, data);
    Check(!state.mOpen, "Close");
    state = Navigate(state, {CommandKind::Open}, data);
    Check(state.mOpen && state.mPage == Page::Home && state.mFirst == 0, "opening again: the start page, at the top");
  });

  Scenario("A tap acts when the finger lifts off the target it pressed (DU-7)", [] {
    const auto data = MakeData(3, 1, std::nullopt);
    const auto layout = BuildLayout({.mOpen = true}, data);
    const int a = FindTarget(layout, CommandKind::TogglePanel, 100);
    const int b = FindTarget(layout, CommandKind::TogglePanel, 101);
    const auto [au, av] = Middle(layout, a);
    const auto [bu, bv] = Middle(layout, b);
    MenuPointer pointer;
    auto r = pointer.Update(layout, au, av, Touch::Hover, 0);
    Check(r.mHover == a && r.mPressed == -1 && !r.mActivated, "hovering: highlighted, nothing done");
    r = pointer.Update(layout, au, av, Touch::Down, 0);
    Check(r.mPressed == a && !r.mActivated, "pressed: shown pressed, nothing done yet");
    r = pointer.Update(layout, au, av, Touch::Hover, 0);
    Check(r.mActivated && r.mActivated->mKind == CommandKind::TogglePanel && r.mActivated->mArg == 100, "lifted on it: it acts");

    pointer.Update(layout, au, av, Touch::Down, 0);
    r = pointer.Update(layout, bu, bv, Touch::Down, 0);
    Check(r.mPressed == -1 && r.mHover == b, "slid onto another target: not shown pressed");
    r = pointer.Update(layout, bu, bv, Touch::Hover, 0);
    Check(!r.mActivated, "lifted on the other target: nothing");

    pointer.Update(layout, au, av, Touch::Down, 0);
    pointer.Update(layout, bu, bv, Touch::Down, 0);
    r = pointer.Update(layout, au, av, Touch::Hover, 0);
    Check(r.mActivated && r.mActivated->mArg == 100, "slid off and back, lifted on it: it acts");

    pointer.Update(layout, 0.5f, 0.995f, Touch::Down, 0);// the bottom margin: no target
    pointer.Update(layout, au, av, Touch::Down, 0);
    r = pointer.Update(layout, au, av, Touch::Hover, 0);
    Check(!r.mActivated, "a press that started on nothing does nothing");

    pointer.Update(layout, au, av, Touch::Down, 0);
    r = pointer.Update(layout, au, av, Touch::Away, 0);
    Check(!r.mActivated && r.mHover == -1, "tracking lost mid-press: nothing");
    r = pointer.Update(layout, au, av, Touch::Hover, 0);
    Check(!r.mActivated, "and nothing when it comes back");

    // Rebuilt mid-press: panel 100 now second (a keybind hid the first)
    pointer.Update(layout, au, av, Touch::Down, 0);
    auto moved = data;
    std::swap(moved.mPanels[0], moved.mPanels[1]);
    const auto rebuilt = BuildLayout({.mOpen = true}, moved);
    r = pointer.Update(rebuilt, au, av, Touch::Hover, 0);
    Check(!r.mActivated, "rebuilt mid-press, lifted where it was: another panel's switch is there now, nothing");
    pointer.Update(layout, au, av, Touch::Down, 0);
    const auto [nu, nv] = Middle(rebuilt, FindTarget(rebuilt, CommandKind::TogglePanel, 100));
    r = pointer.Update(rebuilt, nu, nv, Touch::Hover, 0);
    Check(r.mActivated && r.mActivated->mArg == 100, "rebuilt mid-press, lifted on its own switch's new place: it acts");

    r = pointer.Update(layout, 0.5f, 0.5f, Touch::Hover, 120);
    Check(r.mScrollRows == -1, "two-finger scroll, one notch with the content moving down: a row up");
    pointer.Update(layout, 0.5f, 0.5f, Touch::Hover, -60);
    r = pointer.Update(layout, 0.5f, 0.5f, Touch::Hover, -60);
    Check(r.mScrollRows == 1, "two half notches the other way: a row down");
  });

  Scenario("The shared model wakes the renderer only for picture changes (RN-6)", [] {
    Host::QuickMenuModel model;
    int wakes = 0;
    model.SetWake([&] { ++wakes; });
    const auto layout = std::make_shared<const Layout>(BuildLayout({.mOpen = true}, MakeData(3, 1, std::nullopt)));
    model.SetLayout(layout, false);
    model.SetLayout(std::make_shared<const Layout>(*layout), false);
    Check(wakes == 1, "the same layout twice: one wake");
    model.SetPointer(layout.get(), 2, -1);
    model.SetPointer(layout.get(), 2, -1);
    Check(wakes == 2, "the same hover twice: one wake");
    const Layout other;
    model.SetPointer(&other, 4, 4);
    Check(wakes == 2 && model.Frame().mHover == 2, "a pointer for another layout: ignored");
    model.SetLayout(layout, true);
    Check(wakes == 3 && model.Frame().mHidden && model.Frame().mHover == 2, "hiding: a wake, hover kept (same layout)");
    model.SetLayout(std::make_shared<const Layout>(BuildLayout({}, {})), true);
    Check(wakes == 4 && model.Frame().mHover == -1, "a new layout: a wake, hover found again");
  });

  Scenario("The hand thread's queue: taps in order, scrolls merged, a full queue drops (TH-5)", [] {
    auto model = std::make_shared<Host::QuickMenuModel>();
    const auto layout = std::make_shared<const Layout>(BuildLayout({.mOpen = true}, MakeData(3, 1, std::nullopt)));
    model->SetLayout(layout, false);
    Host::QuickMenuHands hands {model};
    int notified = 0;
    hands.SetNotify([&] { ++notified; });
    Host::TouchPanel panel;
    panel.mID = Host::QuickMenuPanelID;
    using HandInteraction::TouchState;
    const auto tap = [&](int index) {
      const auto [u, v] = Middle(*layout, index);
      hands.Pointer(&panel, {Host::QuickMenuPanelID, u, v, TouchState::Hovering, 0});
      hands.Pointer(&panel, {Host::QuickMenuPanelID, u, v, TouchState::Touching, 0});
      hands.Pointer(&panel, {Host::QuickMenuPanelID, u, v, TouchState::Hovering, 0});
    };
    const int recenter = FindTarget(*layout, CommandKind::Recenter);
    const int toggle = FindTarget(*layout, CommandKind::TogglePanel, 101);
    const auto [ru, rv] = Middle(*layout, recenter);
    hands.Pointer(&panel, {Host::QuickMenuPanelID, ru, rv, TouchState::Touching, 0});
    Check(model->Frame().mPressed == recenter && notified == 0, "pressed: shown at once, nothing queued yet");
    hands.Pointer(&panel, {Host::QuickMenuPanelID, ru, rv, TouchState::Hovering, 0});
    tap(toggle);
    Check(notified == 2, "two taps: the UI thread told twice");
    hands.Pointer(&panel, {Host::QuickMenuPanelID, 0.5f, 0.5f, TouchState::Hovering, -120});
    hands.Pointer(&panel, {Host::QuickMenuPanelID, 0.5f, 0.5f, TouchState::Hovering, -240});
    Check(notified == 3, "two scrolls in a row: told once (merged)");
    auto first = hands.TakeCommand();
    auto second = hands.TakeCommand();
    auto third = hands.TakeCommand();
    Check(first && first->mKind == CommandKind::Recenter && second && second->mKind == CommandKind::TogglePanel
        && second->mArg == 101 && third && third->mKind == CommandKind::Scroll && third->mStep == 3 && !hands.TakeCommand(),
      "taken in order: Recenter, the panel's switch, one scroll of 3 rows");
    hands.Pointer(nullptr, {Host::QuickMenuPanelID, ru, rv, TouchState::Touching, 0});
    hands.Pointer(&panel, {Host::QuickMenuPanelID, ru, rv, TouchState::Hovering, 0});
    Check(!hands.TakeCommand(), "a press on a panel that has gone does nothing");
    for (std::size_t i = 0; i < Host::QuickMenuHands::QueueCapacity + 3; ++i) {
      tap(recenter);
    }
    std::size_t taken = 0;
    while (hands.TakeCommand()) {
      ++taken;
    }
    Check(taken == Host::QuickMenuHands::QueueCapacity && hands.Dropped() == 3,
      std::format("a UI thread that doesn't take them: {} kept, {} dropped and counted", taken, hands.Dropped()));
  });

  Scenario("\"quickMenu\" in settings.json", [] {
    using Host::QuickMenuSettings;
    const auto defaults = Host::QuickMenuConfig::Read(JsonUtil::Json::object());
    Check(defaults == QuickMenuSettings {}, "missing: the defaults (button on, 45 cm, 25 cm below, 35 degrees right, tilted 35)");
    auto odd = JsonUtil::Json::parse(R"({"quickMenu": {"button": false, "distanceCentimeters": 500, "heightCentimeters": -99,
      "turnDegrees": "left", "tiltDegrees": 30, "future": 1}})");
    const auto read = Host::QuickMenuConfig::Read(odd);
    Check(!read.mButton && read.mDistanceCentimeters == QuickMenuSettings::MaxDistance
        && read.mHeightCentimeters == QuickMenuSettings::MinHeight && read.mTurnDegrees == QuickMenuSettings {}.mTurnDegrees
        && read.mTiltDegrees == 30,
      "out of range: the nearest limit; not a number: the default");
    Check(Host::QuickMenuConfig::Read(JsonUtil::Json::parse(R"({"quickMenu": 7})")) == QuickMenuSettings {},
      "not an object: the defaults");
    auto changed = read;
    changed.mTurnDegrees = 20;
    Host::QuickMenuConfig::Write(odd, changed);
    Check(Host::QuickMenuConfig::Read(odd) == changed && odd["quickMenu"].contains("future"),
      "written and read back; a key this version doesn't know is kept");
    QuickMenuSettings bad;
    bad.mDistanceCentimeters = std::numeric_limits<float>::quiet_NaN();
    Check(Host::QuickMenuConfig::Clean(bad).mDistanceCentimeters == QuickMenuSettings {}.mDistanceCentimeters, "NaN: the default");
  });

  Scenario("Its panel: low on the right, sized by its pixels, after the keyboard", [] {
    const auto panel = Host::QuickMenuPanel({});
    const float distance = std::sqrt(
      (panel.mPosition[0] * panel.mPosition[0]) + (panel.mPosition[1] * panel.mPosition[1]) + (panel.mPosition[2] * panel.mPosition[2]));
    Check(panel.mID == Host::QuickMenuPanelID && panel.mKind == Host::PanelKind::QuickMenu && panel.mTouch && panel.mScroll,
      "its fixed ID and kind; tapped and scrolled");
    Check(std::abs(distance - 0.45f) < 0.001f && std::abs(panel.mPosition[1] + 0.25f) < 0.001f && panel.mPosition[0] > 0.1f
        && panel.mPosition[2] < 0 && panel.mYawDegrees == -35 && panel.mPitchDegrees == 35,
      std::format("45 cm away, 25 cm below the eyes, to the right ({:.2f}, {:.2f}, {:.2f}), facing back", panel.mPosition[0],
        panel.mPosition[1], panel.mPosition[2]));
    const auto open = Host::PanelSizeFor(panel, Width, 560);
    const auto closed = Host::PanelSizeFor(panel, ButtonSize, ButtonSize);
    Check(std::abs(open.mWidth - 0.256f) < 0.0001f && std::abs(open.mHeight - 0.224f) < 0.0001f
        && std::abs(closed.mWidth - 0.04f) < 0.0001f,
      "sized by its pixels: 640 x 560 is 256 x 224 mm; the button 40 mm");
    Host::PanelDefinition plain;
    plain.mWidthMeters = 0.5f;
    Check(std::abs(Host::PanelSizeFor(plain, 1000, 500).mHeight - 0.25f) < 0.0001f, "other panels: still their width");
    std::vector<Host::PanelDefinition> enabled(2);
    enabled[0].mID = 1;
    enabled[1].mID = Host::QuickMenuPanelID;// a stray copy: left out
    const Host::KeyboardSettings keyboard;
    const Host::QuickMenuSettings menu;
    const auto all = Host::PanelsForRenderer(enabled, &keyboard, &menu);
    Check(all.size() == 3 && all[0].mID == Host::KeyboardPanelID && all[1].mID == Host::QuickMenuPanelID && all[2].mID == 1,
      "to the renderer: the keyboard, the menu, then the panels");
    Check(Host::PanelsForRenderer(enabled, nullptr, nullptr).size() == 2, "neither: the list as it is");
  });

  return Testing::Finish();
}
