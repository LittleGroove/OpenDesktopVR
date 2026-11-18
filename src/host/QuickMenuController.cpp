// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include "QuickMenuController.hpp"

#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/PanelPlacement.hpp>

#include <OpenDesktopVR/CrashHandler.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <ranges>

namespace OpenDesktopVR::Host {

namespace {

constexpr wchar_t WindowClass[] = L"OpenDesktopVR.QuickMenu";
constexpr UINT CommandsMessage = WM_APP + 30;
constexpr UINT_PTR ListingTimer = 1;
constexpr UINT ListingCheckMs = 100;

using QuickMenu::CommandKind;

static_assert(QuickMenu::KeyboardID == KeyboardPanelID, "the Position page's keyboard is the keyboard panel");

std::string Utf8(const std::wstring& text) {
  return StringUtil::Narrow(text);
}

std::wstring AppName(const WindowUtil::WindowInfo& window) {
  if (window.mExecutablePath.empty()) {
    return {};
  }
  return std::filesystem::path(window.mExecutablePath).stem().wstring();
}

}// namespace

QuickMenuController::QuickMenuController(HINSTANCE instance, PanelManager& panels, RenderLoop& renderer,
  std::shared_ptr<QuickMenuModel> model, QuickMenuHands& hands)
  : mInstance(instance), mPanels(panels), mRenderer(renderer), mModel(std::move(model)), mHands(hands) {
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &CrashHandler::GuardedProcedure<&QuickMenuController::WindowProc>;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = WindowClass;
  RegisterClassExW(&windowClass);
  mWindow = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
  mHands.SetNotify([this] {
    // On the hand thread: hand off, never wait
    if (const HWND window = mWindow.load()) {
      PostMessageW(window, CommandsMessage, 0, 0);
    }
  });
  mChanged = mPanels.Changed().Subscribe([this] { Rebuild(); });
  Rebuild();
  TakeCommands();// any queued before there was a window to tell
}

QuickMenuController::~QuickMenuController() {
  mHands.SetNotify({});
  mChanged.Reset();
  if (const HWND window = mWindow.exchange(nullptr)) {
    KillTimer(window, ListingTimer);
    DestroyWindow(window);
  }
  UnregisterClassW(WindowClass, mInstance);
}

bool QuickMenuController::Toggle(const char* why) {
  if (mState.mOpen) {
    Close(why);
  } else {
    Open(why);
  }
  return mState.mOpen;
}

void QuickMenuController::Open(const char* why) {
  if (!mPanels.Hands().mEnabled) {
    Log::Info("Quick menu: not opened ({}): hands are off, so it couldn't be tapped", why);
    return;
  }
  if (mState.mOpen) {
    return;
  }
  mState = QuickMenu::Navigate(mState, {CommandKind::Open}, mData);
  Log::Info("Quick menu: opened ({})", why);
  Rebuild();
}

void QuickMenuController::Close(const char* why) {
  if (!mState.mOpen) {
    return;
  }
  mState.mOpen = false;
  Log::Info("Quick menu: closed ({})", why);
  Rebuild();
}

void QuickMenuController::TakeCommands() {
  while (const auto command = mHands.TakeCommand()) {
    Run(*command);
  }
}

void QuickMenuController::Run(const QuickMenu::Command& command) {
  // Where the menu goes first, so the host's change lays out the right page
  const bool wasOpen = mState.mOpen;
  mState = QuickMenu::Navigate(mState, command, mData);
  switch (command.mKind) {
    case CommandKind::Open:
      if (!wasOpen) {
        Log::Info("Quick menu: opened (its button)");
      }
      break;
    case CommandKind::Close:
      if (wasOpen) {
        Log::Info("Quick menu: closed (its close button)");
      }
      break;
    case CommandKind::ShowWindows:
      StartListing();
      break;
    case CommandKind::TogglePanel: {
      const auto panel = mPanels.Find(command.mArg);
      if (!panel) {
        break;// DU-2: gone
      }
      const bool showing = mPanels.TogglePanel(command.mArg);
      Log::Info("Quick menu: '{}' {}", Utf8(panel->mName), showing ? "shown" : "hidden");
      break;
    }
    case CommandKind::ToggleAll:
      Log::Info("Quick menu: all panels {}", mPanels.ToggleAll() ? "shown" : "hidden");
      break;
    case CommandKind::Recenter:
      Log::Info("Quick menu: recenter");
      mRenderer.Recenter();
      break;
    case CommandKind::SwitchProfile:
      if (command.mArg < mData.mProfiles.size()) {
        const auto profile = mData.mProfiles[command.mArg];
        if (!mPanels.SwitchProfile(profile.mID, "chosen in the quick menu")) {
          Log::Info("Quick menu: '{}' is already the profile", Utf8(profile.mName));
        }
      }
      break;
    case CommandKind::PickWindow: {
      auto panel = mPanels.Find(mState.mPanel);
      if (!mHaveWindows || command.mArg >= mWindows.size() || !panel || panel->mKind != PanelKind::Window) {
        break;// DU-2: the panel or the list has gone
      }
      const auto& window = mWindows[command.mArg];
      panel->mWindow = WindowMatch::FromWindow(window);
      mPanels.Update(*panel);
      Log::Info("Quick menu: '{}' now shows '{}'", Utf8(panel->mName), Utf8(window.mTitle));
      break;
    }
    case CommandKind::Opacity:
    case CommandKind::Brightness:
    case CommandKind::ToggleTint:
    case CommandKind::Size:
      RunOnPanel(command);
      break;
    case CommandKind::MoveAcross:
    case CommandKind::MoveUp:
    case CommandKind::MoveAway:
    case CommandKind::ToggleFaceMe:
    case CommandKind::Turn:
    case CommandKind::Tilt:
      RunPosition(command);
      break;
    case CommandKind::ToggleKeyboard:
    case CommandKind::UpdateRate:
    case CommandKind::ToggleHandWhilePointing:
    case CommandKind::ToggleButton:
      RunSetting(command);
      break;
    case CommandKind::None:
    case CommandKind::Back:
    case CommandKind::ShowPanelPage:
    case CommandKind::ShowProfiles:
    case CommandKind::ShowSettings:
    case CommandKind::ShowPosition:
    case CommandKind::ToggleSmallSteps:
    case CommandKind::PreviousRows:
    case CommandKind::NextRows:
    case CommandKind::Scroll:
      break;
  }
  Rebuild();
}

void QuickMenuController::RunOnPanel(const QuickMenu::Command& command) {
  auto panel = mPanels.Find(command.mArg);
  if (!panel) {
    return;// DU-2: gone
  }
  const auto name = Utf8(panel->mName);
  const auto step = static_cast<float>(command.mStep);
  switch (command.mKind) {
    case CommandKind::Opacity: {
      const float old = panel->mOpacity;
      const float level = std::clamp(std::round((old + (QuickMenu::LevelStep * step)) * 10.0f) / 10.0f, QuickMenu::MinLevel, 1.0f);
      if (panel->mFocusedOpacity == old) {
        panel->mFocusedOpacity = level;// they went together: so they stay
      }
      panel->mOpacity = level;
      mPanels.Update(*panel);
      Log::Info("Quick menu: '{}' opacity {:.0f}%", name, level * 100);
      return;
    }
    case CommandKind::Brightness:
      if (const auto level = mPanels.ChangeBrightness(command.mArg, QuickMenu::LevelStep * step)) {
        Log::Info("Quick menu: '{}' brightness {:.0f}%", name, *level * 100);
      }
      return;
    case CommandKind::ToggleTint:
      if (const auto tinted = mPanels.ToggleTint(command.mArg)) {
        Log::Info("Quick menu: '{}' tint {}", name, *tinted ? "on" : "off");
      }
      return;
    case CommandKind::Size: {
      if (!(panel->mWidthMeters > 0)) {
        return;
      }
      const float factor = command.mStep > 0 ? QuickMenu::SizeStepFactor : 1.0f / QuickMenu::SizeStepFactor;
      const float width = std::clamp(panel->mWidthMeters * factor, QuickMenu::MinWidthMeters, QuickMenu::MaxWidthMeters);
      const float applied = width / panel->mWidthMeters;
      panel->mWidthMeters = width;
      if (panel->mMaxHeightMeters > 0) {
        panel->mMaxHeightMeters *= applied;// a box (PN-14) keeps its shape
      }
      mPanels.Update(*panel);
      Log::Info("Quick menu: '{}' {:.0f} cm wide", name, width * 100);
      return;
    }
    default:
      return;
  }
}

// The Position page: a panel's, or the keyboard's, placement a step at a
// time, as its fields in the settings window would change it
void QuickMenuController::RunPosition(const QuickMenu::Command& command) {
  using namespace QuickMenu;
  const float metres = (mState.mSmallSteps ? SmallStepMeters : StepMeters) * static_cast<float>(command.mStep);
  const float degrees = (mState.mSmallSteps ? SmallStepDegrees : StepDegrees) * static_cast<float>(command.mStep);
  const auto move = [&](PanelPlacement& p) -> std::string {
    using PanelPlacementConfig::Nudge;
    using PanelPlacementConfig::NudgeBy;
    switch (command.mKind) {
      case CommandKind::MoveAcross:
        NudgeBy(p, Nudge::Across, metres);
        return std::format("{:.1f} cm left or right", p.mPosition[0] * 100);
      case CommandKind::MoveUp:
        NudgeBy(p, Nudge::Up, metres);
        return std::format("{:.1f} cm down or up", p.mPosition[1] * 100);
      case CommandKind::MoveAway:
        NudgeBy(p, Nudge::Away, metres);
        return std::format("{:.1f} cm away", -p.mPosition[2] * 100);
      case CommandKind::ToggleFaceMe:
        if (p.mFaceOrigin) {
          // Start from the way it faces now, so it doesn't jump (as the
          // settings window does)
          const auto facing = FacingOrigin(p.mPosition.data());
          p.mYawDegrees = facing.mYawDegrees;
          p.mPitchDegrees = facing.mPitchDegrees;
        }
        p.mFaceOrigin = !p.mFaceOrigin;
        return p.mFaceOrigin ? "facing you" : "set by hand";
      case CommandKind::Turn:
        NudgeBy(p, Nudge::Turn, degrees);
        return std::format("turned {:.0f} degrees", p.mYawDegrees);
      case CommandKind::Tilt:
        NudgeBy(p, Nudge::Tilt, degrees);
        return std::format("tilted {:.0f} degrees", p.mPitchDegrees);
      default:
        return {};
    }
  };
  if (command.mArg == KeyboardPanelID) {
    auto keyboard = mPanels.Keyboard();
    const auto what = move(keyboard.mPlacement);
    mPanels.SetKeyboard(keyboard);
    Log::Info("Quick menu: the keyboard {}", what);
    return;
  }
  auto panel = mPanels.Find(command.mArg);
  if (!panel) {
    return;// DU-2: gone
  }
  auto placement = PanelPlacementConfig::From(*panel);
  const auto what = move(placement);
  PanelPlacementConfig::ApplyTo(placement, *panel);
  mPanels.Update(*panel);
  Log::Info("Quick menu: '{}' {}", Utf8(panel->mName), what);
}

// The keyboard shows while its Position page is open, and goes when that
// closes if the page was what showed it
void QuickMenuController::PreviewKeyboard() {
  const bool wanted = mState.mOpen && mState.mPage == QuickMenu::Page::Position && mState.mPanel == KeyboardPanelID
    && mPanels.Keyboard().mEnabled;
  if (wanted == mPreviewing || !mKeyboardPreview) {
    return;
  }
  mPreviewing = wanted;
  if (wanted) {
    mPreviewShowed = mKeyboardPreview(true);
  } else if (std::exchange(mPreviewShowed, false)) {
    mKeyboardPreview(false);
  }
}

void QuickMenuController::RunSetting(const QuickMenu::Command& command) {
  switch (command.mKind) {
    case CommandKind::ToggleKeyboard: {
      auto keyboard = mPanels.Keyboard();
      keyboard.mEnabled = !keyboard.mEnabled;
      mPanels.SetKeyboard(keyboard);
      Log::Info("Quick menu: the virtual keyboard {}", keyboard.mEnabled ? "on" : "off");
      return;
    }
    case CommandKind::UpdateRate: {
      const auto& rates = PanelManager::UpdateRates;
      const int current = static_cast<int>(std::lround(mPanels.MaxUpdatesPerSecond()));
      std::optional<int> next;
      if (command.mStep > 0) {
        if (const auto it = std::ranges::find_if(rates, [&](int rate) { return rate > current; }); it != rates.end()) {
          next = *it;
        }
      } else if (const auto it = std::ranges::find_if(rates | std::views::reverse, [&](int rate) { return rate < current; });
                 it != (rates | std::views::reverse).end()) {
        next = *it;
      }
      if (next) {
        mPanels.SetMaxUpdatesPerSecond(*next);
        Log::Info("Quick menu: panels update at most {} times a second", *next);
      }
      return;
    }
    case CommandKind::ToggleHandWhilePointing: {
      auto hands = mPanels.Hands();
      hands.mShowWhilePointing = !hands.mShowWhilePointing;
      mPanels.SetHands(hands);
      Log::Info("Quick menu: the hand while pointing {}", hands.mShowWhilePointing ? "shown" : "not shown");
      return;
    }
    case CommandKind::ToggleButton: {
      auto menu = mPanels.QuickMenu();
      menu.mButton = !menu.mButton;
      mPanels.SetQuickMenu(menu);
      Log::Info("Quick menu: its button {}", menu.mButton ? "on" : "off (its keybind opens it)");
      return;
    }
    default:
      return;
  }
}

QuickMenu::MenuData QuickMenuController::Gather() const {
  QuickMenu::MenuData data;
  for (const auto& panel: mPanels.Panels()) {
    if (!panel.mEnabled) {
      continue;// off at the desk: not in VR at all
    }
    data.mPanels.push_back({
      .mID = panel.mID,
      .mName = panel.mName,
      .mWindow = panel.mKind == PanelKind::Window,
      .mShowing = !mPanels.IsHidden(panel.mID),
      .mOpacity = panel.mOpacity,
      .mBrightness = panel.mBrightness,
      .mTint = panel.mTintEnabled,
      .mWidthMeters = panel.mWidthMeters,
      .mPosition = {panel.mPosition[0], panel.mPosition[1], panel.mPosition[2]},
      .mFaceOrigin = panel.mFaceOrigin,
      .mYawDegrees = panel.mYawDegrees,
      .mPitchDegrees = panel.mPitchDegrees,
    });
  }
  const auto& keyboard = mPanels.Keyboard().mPlacement;
  data.mKeyboardPlacement.mPosition = keyboard.mPosition;
  data.mKeyboardPlacement.mFaceOrigin = keyboard.mFaceOrigin;
  data.mKeyboardPlacement.mYawDegrees = keyboard.mYawDegrees;
  data.mKeyboardPlacement.mPitchDegrees = keyboard.mPitchDegrees;
  for (const auto& profile: mPanels.Profiles()) {
    const bool active = profile.mID == mPanels.ActiveProfile();
    data.mProfiles.push_back({profile.mID, profile.mName, active});
    if (active) {
      data.mProfileName = profile.mName;
    }
  }
  data.mAnyShowing = mPanels.AnyShowing();
  data.mKeyboard = mPanels.Keyboard().mEnabled;
  const int rate = static_cast<int>(std::lround(mPanels.MaxUpdatesPerSecond()));
  data.mUpdateRate = rate;
  data.mSlowerRate = PanelManager::UpdateRates.front() < rate;
  data.mFasterRate = PanelManager::UpdateRates.back() > rate;
  data.mHandWhilePointing = mPanels.Hands().mShowWhilePointing;
  data.mButton = mPanels.QuickMenu().mButton;
  if (mHaveWindows) {
    std::vector<QuickMenu::WindowEntry> windows;
    windows.reserve(mWindows.size());
    for (const auto& window: mWindows) {
      windows.push_back({window.mTitle, AppName(window)});
    }
    data.mWindows = std::move(windows);
  }
  return data;
}

void QuickMenuController::Rebuild() {
  if (mRebuilding) {
    return;// a change made while laying out (none expected): the outer call has it
  }
  mRebuilding = true;
  if (!mPanels.Hands().mEnabled) {
    mState.mOpen = false;// no hands, no taps: its panel has gone too
  }
  if (mState.mOpen) {
    mData = Gather();
  }
  const bool hidden = !mState.mOpen && !mPanels.QuickMenu().mButton;
  mModel->SetLayout(std::make_shared<const QuickMenu::Layout>(QuickMenu::BuildLayout(mState, mData)), hidden);
  PreviewKeyboard();
  mRebuilding = false;
}

void QuickMenuController::StartListing() {
  mHaveWindows = false;
  mWindows.clear();
  if (mListing.Running()) {
    return;
  }
  try {
    mListing = ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>>::Start(
      L"Quick menu windows", [] { return WindowUtil::ListCaptureCandidates(); });
  } catch (const std::exception& e) {
    Log::Warning("Quick menu: couldn't list windows: {}", e.what());
    mHaveWindows = true;// shows "No windows to show" rather than waiting forever
    return;
  }
  if (const HWND window = mWindow.load()) {
    SetTimer(window, ListingTimer, ListingCheckMs, nullptr);
  }
}

void QuickMenuController::CheckListing() {
  if (mListing.Running()) {
    return;
  }
  if (const HWND window = mWindow.load()) {
    KillTimer(window, ListingTimer);
  }
  mWindows = mListing.Take().value_or(std::vector<WindowUtil::WindowInfo> {});
  mHaveWindows = true;
  Rebuild();
}

LRESULT CALLBACK QuickMenuController::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
  }
  auto* self = reinterpret_cast<QuickMenuController*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self && message == CommandsMessage) {
    ThreadUtil::CatchAll("quick menu", [&] { self->TakeCommands(); });
    return 0;
  }
  if (self && message == WM_TIMER && wParam == ListingTimer) {
    ThreadUtil::CatchAll("quick menu windows", [&] { self->CheckListing(); });
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}// namespace OpenDesktopVR::Host
