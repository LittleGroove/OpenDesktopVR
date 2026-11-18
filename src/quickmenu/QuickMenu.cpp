// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/QuickMenu.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace OpenDesktopVR::QuickMenu {

namespace {

// In the menu's pixels (2.5 a millimetre)
constexpr float Margin = 12;
constexpr float Gap = 8;
constexpr float RowHeight = 60;   // 24 mm
constexpr float SideButton = 76;  // back and close: 30 mm
constexpr float ToggleWidth = 130;// 52 mm
constexpr float StepperWidth = 76;
constexpr float PagingWidth = 180;
constexpr float Indent = 8;// labels and the title sit a little in
constexpr float W = static_cast<float>(Width);
// Comparisons with stepped floats
constexpr float Epsilon = 0.001f;

class Builder {
 public:
  Rect NextRow() {
    const Rect row {Margin, mY, W - Margin, mY + RowHeight};
    mY += RowHeight + Gap;
    return row;
  }
  void Add(ItemKind kind, Rect rect, std::wstring text, Command command = {}, std::wstring value = {}, bool on = false,
    bool enabled = true) {
    mLayout.mItems.push_back({kind, rect, std::move(text), std::move(value), command, on, enabled});
  }
  void Space(float pixels) {
    mY += pixels;
  }
  Layout Finish() && {
    mLayout.mWidth = Width;
    mLayout.mHeight = static_cast<uint32_t>(std::ceil(mY - Gap + Margin));
    return std::move(mLayout);
  }

 private:
  Layout mLayout;
  float mY {Margin};
};

std::wstring NameOf(const PanelInfo& panel) {
  return panel.mName.empty() ? L"(no name)" : panel.mName;
}

std::wstring Percent(float level) {
  return std::format(L"{}%", std::lround(level * 100.0f));
}

std::wstring Centimetres(float meters) {
  return std::format(L"{} cm", std::lround(meters * 100.0f));
}

// To a tenth, without ".0": "12 cm", "12.5 cm", "-3 cm" (the settings
// window's fields can leave tenths)
std::wstring Tenths(float value, const wchar_t* unit) {
  const float tenths = std::round(value * 10.0f) / 10.0f;
  if (std::abs(tenths - std::round(tenths)) < 0.05f) {
    return std::format(L"{}{}", std::lround(tenths), unit);
  }
  return std::format(L"{:.1f}{}", tenths, unit);
}

std::wstring Length(float meters) {
  return Tenths(meters * 100.0f, L" cm");
}

std::wstring Angle(float degrees) {
  return Tenths(degrees, L"°");
}

std::size_t ClampFirst(std::size_t first, std::size_t count) noexcept {
  return count > RowsPerPage ? std::min(first, count - RowsPerPage) : 0;
}

void Header(Builder& b, std::wstring title, bool back) {
  const Rect row = b.NextRow();
  float left = row.mLeft + Indent;
  if (back) {
    b.Add(ItemKind::Button, {row.mLeft, row.mTop, row.mLeft + SideButton, row.mBottom}, L"‹", {CommandKind::Back});
    left = row.mLeft + SideButton + (2 * Gap);
  }
  b.Add(ItemKind::Title, {left, row.mTop, row.mRight - SideButton - Gap, row.mBottom}, std::move(title));
  b.Add(ItemKind::Button, {row.mRight - SideButton, row.mTop, row.mRight, row.mBottom}, L"×", {CommandKind::Close});
  b.Space(Gap / 2);
}

// Previous and next, around "6-10 of 12", if the list doesn't fit
void Paging(Builder& b, std::size_t first, std::size_t count) {
  if (count <= RowsPerPage) {
    return;
  }
  const Rect row = b.NextRow();
  const auto last = std::min(first + RowsPerPage, count);
  b.Add(ItemKind::Button, {row.mLeft, row.mTop, row.mLeft + PagingWidth, row.mBottom}, L"‹ Previous",
    {CommandKind::PreviousRows}, {}, false, first > 0);
  b.Add(ItemKind::Note, {row.mLeft + PagingWidth + Gap, row.mTop, row.mRight - PagingWidth - Gap, row.mBottom},
    std::format(L"{}–{} of {}", first + 1, last, count));
  b.Add(ItemKind::Button, {row.mRight - PagingWidth, row.mTop, row.mRight, row.mBottom}, L"Next ›",
    {CommandKind::NextRows}, {}, false, last < count);
}

void ToggleRow(Builder& b, std::wstring label, bool on, Command command) {
  const Rect row = b.NextRow();
  b.Add(ItemKind::Label, {row.mLeft + Indent, row.mTop, row.mRight - ToggleWidth - Gap, row.mBottom}, std::move(label));
  b.Add(ItemKind::Toggle, {row.mRight - ToggleWidth, row.mTop, row.mRight, row.mBottom}, L"", command, {}, on);
}

void StepRow(Builder& b, std::wstring label, std::wstring value, Command command, bool canLess, bool canMore) {
  const Rect row = b.NextRow();
  const float plusLeft = row.mRight - StepperWidth;
  const float minusLeft = plusLeft - Gap - StepperWidth;
  b.Add(ItemKind::Label, {row.mLeft + Indent, row.mTop, minusLeft - Gap, row.mBottom}, std::move(label), {}, std::move(value));
  command.mStep = -1;
  b.Add(ItemKind::Stepper, {minusLeft, row.mTop, minusLeft + StepperWidth, row.mBottom}, L"−", command, {}, false, canLess);
  command.mStep = 1;
  b.Add(ItemKind::Stepper, {plusLeft, row.mTop, row.mRight, row.mBottom}, L"+", command, {}, false, canMore);
}

Layout Closed() {
  Layout layout {ButtonSize, ButtonSize, {}};
  layout.mItems.push_back({.mKind = ItemKind::Icon,
    .mRect = {0, 0, static_cast<float>(ButtonSize), static_cast<float>(ButtonSize)},
    .mCommand = {CommandKind::Open}});
  return layout;
}

Layout Home(const MenuState& state, const MenuData& data) {
  Builder b;
  Header(b, data.mProfileName.empty() ? std::wstring {L"Panels"} : L"Profile: " + data.mProfileName, false);
  const auto count = data.mPanels.size();
  const auto first = ClampFirst(state.mFirst, count);
  if (count == 0) {
    b.Add(ItemKind::Note, b.NextRow(), L"No panels yet: add them in the settings window");
  }
  for (std::size_t i = first; i < std::min(first + RowsPerPage, count); ++i) {
    const auto& panel = data.mPanels[i];
    const Rect row = b.NextRow();
    b.Add(ItemKind::Row, {row.mLeft, row.mTop, row.mRight - ToggleWidth - Gap, row.mBottom}, NameOf(panel),
      {CommandKind::ShowPanelPage, panel.mID});
    b.Add(ItemKind::Toggle, {row.mRight - ToggleWidth, row.mTop, row.mRight, row.mBottom}, L"",
      {CommandKind::TogglePanel, panel.mID}, {}, panel.mShowing);
  }
  Paging(b, first, count);
  b.Space(Gap);
  const float half = (W - (2 * Margin) - Gap) / 2;
  Rect row = b.NextRow();
  b.Add(ItemKind::Button, {row.mLeft, row.mTop, row.mLeft + half, row.mBottom}, L"Recenter", {CommandKind::Recenter});
  b.Add(ItemKind::Button, {row.mRight - half, row.mTop, row.mRight, row.mBottom},
    data.mAnyShowing ? L"Hide all" : L"Show all", {CommandKind::ToggleAll}, {}, false, count > 0);
  row = b.NextRow();
  b.Add(ItemKind::Row, {row.mLeft, row.mTop, row.mLeft + half, row.mBottom}, L"Profiles", {CommandKind::ShowProfiles});
  b.Add(ItemKind::Row, {row.mRight - half, row.mTop, row.mRight, row.mBottom}, L"Settings", {CommandKind::ShowSettings});
  return std::move(b).Finish();
}

Layout PanelPage(const MenuState& state, const MenuData& data) {
  Builder b;
  const auto panel = std::ranges::find(data.mPanels, state.mPanel, &PanelInfo::mID);
  if (panel == data.mPanels.end()) {
    // DU-2: gone (another profile, removed at the desk)
    Header(b, L"Panel", true);
    b.Add(ItemKind::Note, b.NextRow(), L"This panel isn't in the profile any more");
    return std::move(b).Finish();
  }
  const uint64_t id = panel->mID;
  Header(b, NameOf(*panel), true);
  ToggleRow(b, L"Show in VR", panel->mShowing, {CommandKind::TogglePanel, id});
  StepRow(b, L"Opacity", Percent(panel->mOpacity), {CommandKind::Opacity, id}, panel->mOpacity > MinLevel + Epsilon,
    panel->mOpacity < 1.0f - Epsilon);
  StepRow(b, L"Brightness", Percent(panel->mBrightness), {CommandKind::Brightness, id},
    panel->mBrightness > MinLevel + Epsilon, panel->mBrightness < 1.0f - Epsilon);
  ToggleRow(b, L"Tint", panel->mTint, {CommandKind::ToggleTint, id});
  StepRow(b, L"Size", Centimetres(panel->mWidthMeters) + L" wide", {CommandKind::Size, id},
    panel->mWidthMeters > MinWidthMeters + Epsilon, panel->mWidthMeters < MaxWidthMeters - Epsilon);
  b.Add(ItemKind::Row, b.NextRow(), L"Position", {CommandKind::ShowPosition, id});
  if (panel->mWindow) {
    b.Add(ItemKind::Row, b.NextRow(), L"Change window", {CommandKind::ShowWindows, id});
  }
  return std::move(b).Finish();
}

// Test round 19: the settings window's placement, a step a tap
Layout PositionPage(const MenuState& state, const MenuData& data) {
  Builder b;
  const bool keyboard = state.mPanel == KeyboardID;
  const PanelInfo* target = nullptr;
  if (keyboard) {
    target = data.mKeyboard ? &data.mKeyboardPlacement : nullptr;
  } else if (const auto panel = std::ranges::find(data.mPanels, state.mPanel, &PanelInfo::mID); panel != data.mPanels.end()) {
    target = &*panel;
  }
  if (!target) {
    Header(b, L"Position", true);
    b.Add(ItemKind::Note, b.NextRow(), keyboard ? L"The virtual keyboard is off" : L"This panel isn't in the profile any more");
    return std::move(b).Finish();
  }
  const uint64_t id = target->mID;
  Header(b, keyboard ? std::wstring {L"Keyboard position"} : NameOf(*target) + L": position", true);
  const auto& at = target->mPosition;
  StepRow(b, L"Left / right", Length(at[0]), {CommandKind::MoveAcross, id}, at[0] > -MaxAcrossMeters + Epsilon,
    at[0] < MaxAcrossMeters - Epsilon);
  StepRow(b, L"Down / up", Length(at[1]), {CommandKind::MoveUp, id}, at[1] > -MaxUpDownMeters + Epsilon,
    at[1] < MaxUpDownMeters - Epsilon);
  const float away = -at[2];
  StepRow(b, L"Distance", Length(away), {CommandKind::MoveAway, id}, away > MinAwayMeters + Epsilon,
    away < MaxAwayMeters - Epsilon);
  ToggleRow(b, L"Face me", target->mFaceOrigin, {CommandKind::ToggleFaceMe, id});
  if (!target->mFaceOrigin) {
    StepRow(b, L"Turn", Angle(target->mYawDegrees), {CommandKind::Turn, id}, true, true);// it wraps round
    StepRow(b, L"Tilt", Angle(target->mPitchDegrees), {CommandKind::Tilt, id},
      target->mPitchDegrees > -MaxTiltDegrees + Epsilon, target->mPitchDegrees < MaxTiltDegrees - Epsilon);
  }
  ToggleRow(b, L"Small steps (1 cm, 1°)", state.mSmallSteps, {CommandKind::ToggleSmallSteps});
  return std::move(b).Finish();
}

Layout WindowsPage(const MenuState& state, const MenuData& data) {
  Builder b;
  Header(b, L"Choose a window", true);
  if (!data.mWindows) {
    b.Add(ItemKind::Note, b.NextRow(), L"Finding windows...");
    return std::move(b).Finish();
  }
  const auto& windows = *data.mWindows;
  if (windows.empty()) {
    b.Add(ItemKind::Note, b.NextRow(), L"No windows to show");
  }
  const auto first = ClampFirst(state.mFirst, windows.size());
  for (std::size_t i = first; i < std::min(first + RowsPerPage, windows.size()); ++i) {
    b.Add(ItemKind::Row, b.NextRow(), windows[i].mTitle.empty() ? L"(no title)" : windows[i].mTitle,
      {CommandKind::PickWindow, i}, windows[i].mApp);
  }
  Paging(b, first, windows.size());
  return std::move(b).Finish();
}

Layout ProfilesPage(const MenuState& state, const MenuData& data) {
  Builder b;
  Header(b, L"Profiles", true);
  const auto& profiles = data.mProfiles;
  const auto first = ClampFirst(state.mFirst, profiles.size());
  for (std::size_t i = first; i < std::min(first + RowsPerPage, profiles.size()); ++i) {
    b.Add(ItemKind::Row, b.NextRow(), profiles[i].mName.empty() ? L"(no name)" : profiles[i].mName,
      {CommandKind::SwitchProfile, i}, {}, profiles[i].mActive);
  }
  Paging(b, first, profiles.size());
  return std::move(b).Finish();
}

Layout SettingsPage(const MenuData& data) {
  Builder b;
  Header(b, L"Settings", true);
  ToggleRow(b, L"Virtual keyboard", data.mKeyboard, {CommandKind::ToggleKeyboard});
  b.Add(ItemKind::Row, b.NextRow(), L"Keyboard position", {CommandKind::ShowPosition, KeyboardID}, {}, false, data.mKeyboard);
  StepRow(b, L"Panel updates", std::format(L"{} a second", data.mUpdateRate), {CommandKind::UpdateRate}, data.mSlowerRate,
    data.mFasterRate);
  ToggleRow(b, L"Hand while pointing", data.mHandWhilePointing, {CommandKind::ToggleHandWhilePointing});
  ToggleRow(b, L"Menu button", data.mButton, {CommandKind::ToggleButton});
  if (!data.mButton) {
    b.Add(ItemKind::Note, b.NextRow(), L"Off: only the menu's keybind opens it");
  }
  return std::move(b).Finish();
}

}// namespace

std::string Describe(const Command& command) {
  const char* name = "?";
  switch (command.mKind) {
    case CommandKind::None:
      name = "none";
      break;
    case CommandKind::Open:
      name = "open";
      break;
    case CommandKind::Close:
      name = "close";
      break;
    case CommandKind::Back:
      name = "back";
      break;
    case CommandKind::ShowPanelPage:
      name = "panel page";
      break;
    case CommandKind::ShowWindows:
      name = "windows";
      break;
    case CommandKind::ShowPosition:
      name = "position";
      break;
    case CommandKind::ShowProfiles:
      name = "profiles";
      break;
    case CommandKind::ShowSettings:
      name = "settings";
      break;
    case CommandKind::PreviousRows:
      name = "previous rows";
      break;
    case CommandKind::NextRows:
      name = "next rows";
      break;
    case CommandKind::Scroll:
      name = "scroll";
      break;
    case CommandKind::TogglePanel:
      name = "show or hide a panel";
      break;
    case CommandKind::ToggleAll:
      name = "show or hide all";
      break;
    case CommandKind::Recenter:
      name = "recenter";
      break;
    case CommandKind::SwitchProfile:
      name = "switch profile";
      break;
    case CommandKind::PickWindow:
      name = "pick a window";
      break;
    case CommandKind::Opacity:
      name = "opacity";
      break;
    case CommandKind::Brightness:
      name = "brightness";
      break;
    case CommandKind::ToggleTint:
      name = "tint";
      break;
    case CommandKind::Size:
      name = "size";
      break;
    case CommandKind::MoveAcross:
      name = "left or right";
      break;
    case CommandKind::MoveUp:
      name = "down or up";
      break;
    case CommandKind::MoveAway:
      name = "distance";
      break;
    case CommandKind::ToggleFaceMe:
      name = "face me";
      break;
    case CommandKind::Turn:
      name = "turn";
      break;
    case CommandKind::Tilt:
      name = "tilt";
      break;
    case CommandKind::ToggleSmallSteps:
      name = "small steps";
      break;
    case CommandKind::ToggleKeyboard:
      name = "keyboard";
      break;
    case CommandKind::UpdateRate:
      name = "update rate";
      break;
    case CommandKind::ToggleHandWhilePointing:
      name = "hand while pointing";
      break;
    case CommandKind::ToggleButton:
      name = "menu button";
      break;
  }
  return command.mStep == 0 ? std::string {name} : std::format("{} {:+d}", name, command.mStep);
}

Layout BuildLayout(const MenuState& state, const MenuData& data) {
  if (!state.mOpen) {
    return Closed();
  }
  switch (state.mPage) {
    case Page::Home:
      return Home(state, data);
    case Page::Panel:
      return PanelPage(state, data);
    case Page::Windows:
      return WindowsPage(state, data);
    case Page::Profiles:
      return ProfilesPage(state, data);
    case Page::Settings:
      return SettingsPage(data);
    case Page::Position:
      return PositionPage(state, data);
  }
  return Home(state, data);
}

int HitTest(const Layout& layout, float x, float y) noexcept {
  for (auto i = static_cast<int>(layout.mItems.size()) - 1; i >= 0; --i) {
    const auto& item = layout.mItems[static_cast<std::size_t>(i)];
    if (item.IsTarget() && item.mRect.Contains(x, y)) {
      return i;
    }
  }
  return -1;
}

std::size_t ListLength(const MenuState& state, const MenuData& data) noexcept {
  switch (state.mPage) {
    case Page::Home:
      return data.mPanels.size();
    case Page::Windows:
      return data.mWindows ? data.mWindows->size() : 0;
    case Page::Profiles:
      return data.mProfiles.size();
    case Page::Panel:
    case Page::Settings:
    case Page::Position:
      return 0;
  }
  return 0;
}

MenuState Navigate(MenuState state, const Command& command, const MenuData& data) {
  switch (command.mKind) {
    case CommandKind::Open:
      return MenuState {.mOpen = true};
    case CommandKind::Close:
      state.mOpen = false;
      return state;
    case CommandKind::Back:
      if (state.mPage == Page::Position) {
        state.mPage = state.mPanel == KeyboardID ? Page::Settings : Page::Panel;
      } else {
        state.mPage = state.mPage == Page::Windows ? Page::Panel : Page::Home;
      }
      state.mFirst = 0;
      return state;
    case CommandKind::ShowPosition:
      state.mPage = Page::Position;
      state.mPanel = command.mArg;
      state.mFirst = 0;
      return state;
    case CommandKind::ToggleSmallSteps:
      state.mSmallSteps = !state.mSmallSteps;
      return state;
    case CommandKind::ShowPanelPage:
      state.mPage = Page::Panel;
      state.mPanel = command.mArg;
      state.mFirst = 0;
      return state;
    case CommandKind::ShowWindows:
      state.mPage = Page::Windows;
      state.mFirst = 0;
      return state;
    case CommandKind::ShowProfiles:
      state.mPage = Page::Profiles;
      state.mFirst = 0;
      return state;
    case CommandKind::ShowSettings:
      state.mPage = Page::Settings;
      state.mFirst = 0;
      return state;
    case CommandKind::SwitchProfile:
      state.mPage = Page::Home;
      state.mFirst = 0;
      return state;
    case CommandKind::PickWindow:
      state.mPage = Page::Panel;
      state.mFirst = 0;
      return state;
    case CommandKind::PreviousRows:
      state.mFirst = state.mFirst > RowsPerPage ? state.mFirst - RowsPerPage : 0;
      break;
    case CommandKind::NextRows:
      state.mFirst += RowsPerPage;
      break;
    case CommandKind::Scroll: {
      const auto rows = static_cast<std::size_t>(std::abs(command.mStep));
      state.mFirst = command.mStep < 0 ? (state.mFirst > rows ? state.mFirst - rows : 0) : state.mFirst + rows;
      break;
    }
    default:
      return state;
  }
  state.mFirst = ClampFirst(state.mFirst, ListLength(state, data));
  return state;
}

PointerResult MenuPointer::Update(const Layout& layout, float u, float v, Touch touch, int32_t wheel) {
  PointerResult result;
  const float x = u * static_cast<float>(layout.mWidth);
  const float y = v * static_cast<float>(layout.mHeight);
  const int under = touch == Touch::Away ? -1 : HitTest(layout, x, y);
  const auto commandUnder = [&]() -> std::optional<Command> {
    if (under < 0) {
      return std::nullopt;
    }
    return layout.mItems[static_cast<std::size_t>(under)].mCommand;
  };
  if (touch == Touch::Down) {
    if (!mDown) {
      mDown = true;
      mPressed = commandUnder();// may be none: then nothing acts
    }
  } else if (mDown) {
    // Lifted: acts only on the target it pressed (DU-7)
    mDown = false;
    if (mPressed && commandUnder() == mPressed) {
      result.mActivated = mPressed;
    }
    mPressed.reset();
  }
  result.mHover = under;
  result.mPressed = (mDown && mPressed && commandUnder() == mPressed) ? under : -1;
  if (wheel != 0) {
    // Positive wheel moves the content down: earlier rows come into view
    mWheel += wheel;
    const int32_t notches = mWheel / 120;
    mWheel -= notches * 120;
    result.mScrollRows = -notches;
  }
  return result;
}

void MenuPointer::Reset() noexcept {
  mDown = false;
  mPressed.reset();
  mWheel = 0;
}

}// namespace OpenDesktopVR::QuickMenu
