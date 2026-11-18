// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include "AutomationService.hpp"

#include <OpenDesktopVR/CrashHandler.hpp>
#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PanelPlacement.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace OpenDesktopVR::Host {

namespace {

constexpr UINT RunMessage = WM_APP + 40;
constexpr UINT_PTR ListingTimer = 1;
constexpr UINT ListingCheckMs = 100;
constexpr std::size_t MaxWindowRequests = 8;
// WM_COPYDATA's reply: the Reply, offset so a window that ignored the
// message (0) isn't read as "done"
constexpr LRESULT ReplyBase = 100;
constexpr UINT SendTimeoutMs = 3000;

using Automation::Command;
using Automation::Reply;
using Automation::Switch;
using Automation::Target;
using Automation::Verb;

std::string Utf8(const std::wstring& text) {
  return StringUtil::Narrow(text);
}

}// namespace

std::wstring AutomationWindowClass() {
  // As the host's kernel objects are named (a test instance has its own),
  // without the "Local\" namespace, which window classes don't have
  auto name = SharedObjectName(L"Automation", SharedAbi::AbiVersion);
  if (const auto slash = name.find(L'\\'); slash != std::wstring::npos) {
    name.erase(0, slash + 1);
  }
  return name;
}

int SendCommand(std::span<const std::wstring> words) {
  const auto parsed = Automation::Parse(words);
  if (parsed.mCommand && parsed.mCommand->mVerb == Verb::Help) {
    const auto usage = Automation::Usage();
    MessageBoxW(nullptr, usage.c_str(), L"Open Desktop VR commands", MB_OK | MB_ICONINFORMATION);
    return static_cast<int>(Reply::Done);
  }
  // Even a command it can't read goes to the host, whose log says why
  const HWND host = FindWindowExW(HWND_MESSAGE, nullptr, AutomationWindowClass().c_str(), nullptr);
  if (!host) {
    return static_cast<int>(Reply::NotRunning);
  }
  auto packed = Automation::Pack(words);
  COPYDATASTRUCT data {Automation::WireTag, static_cast<DWORD>(packed.size() * sizeof(wchar_t)), packed.data()};
  DWORD_PTR result = 0;
  if (!SendMessageTimeoutW(host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, SendTimeoutMs, &result)) {
    return static_cast<int>(Reply::NoAnswer);
  }
  const auto reply = static_cast<LRESULT>(result) - ReplyBase;
  return reply >= 0 && reply <= static_cast<LRESULT>(Reply::NoAnswer) ? static_cast<int>(reply) : static_cast<int>(Reply::NoAnswer);
}

AutomationService::AutomationService(
  HINSTANCE instance, PanelManager& panels, RenderLoop& renderer, KeyboardService& keyboard, QuickMenuController& quickMenu)
  : mInstance(instance), mPanels(panels), mRenderer(renderer), mKeyboard(keyboard), mQuickMenu(quickMenu),
    mClass(AutomationWindowClass()) {
  WNDCLASSEXW windowClass {sizeof(windowClass)};
  windowClass.lpfnWndProc = &CrashHandler::GuardedProcedure<&AutomationService::WindowProc>;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = mClass.c_str();
  RegisterClassExW(&windowClass);
  mWindow = CreateWindowExW(0, mClass.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
  if (!mWindow) {
    Log::Warning("Commands: couldn't make the window they're sent to ({}); Stream Deck and similar won't reach this app",
      GetLastError());
  }
}

AutomationService::~AutomationService() {
  if (mWindow) {
    KillTimer(mWindow, ListingTimer);
    DestroyWindow(mWindow);
  }
  UnregisterClassW(mClass.c_str(), mInstance);
}

void AutomationService::Refused(const std::string& why) {
  // At most 10 a minute in the log: a tool sending nonsense in a loop
  // mustn't fill it
  const auto now = std::chrono::steady_clock::now();
  if (now - mRefusalMinute >= std::chrono::minutes(1)) {
    if (mRefusalsQuiet > 0) {
      Log::Warning("Commands: {} more were refused in the last minute (not logged one by one)", mRefusalsQuiet);
    }
    mRefusalMinute = now;
    mRefusalsLogged = 0;
    mRefusalsQuiet = 0;
  }
  if (mRefusalsLogged < 10) {
    ++mRefusalsLogged;
    Log::Warning("Command refused: {}", why);
  } else {
    ++mRefusalsQuiet;
  }
}

std::optional<AutomationService::Ready> AutomationService::Prepare(const Command& command, std::string& why) const {
  Ready ready {command, {}, {}};
  if (command.mTarget.mKind == Target::Kind::Panel) {
    std::vector<Automation::PanelName> names;
    for (const auto& panel: mPanels.Panels()) {
      names.push_back({panel.mID, panel.mName});
    }
    ready.mPanels = Automation::Resolve(command.mTarget.mSelector, names);
    if (ready.mPanels.empty()) {
      why = std::format("{}: no panel called '{}' in this profile", Automation::Describe(command), Utf8(command.mTarget.mSelector));
      return std::nullopt;
    }
  } else if (command.mTarget.mKind == Target::Kind::All) {
    for (const auto& panel: mPanels.Panels()) {
      if (panel.mEnabled) {
        ready.mPanels.push_back(panel.mID);
      }
    }
  }
  if (command.mVerb == Verb::Window) {
    for (const auto id: ready.mPanels) {
      if (const auto panel = mPanels.Find(id); !panel || panel->mKind != PanelKind::Window) {
        why = std::format("{}: '{}' doesn't show a window", Automation::Describe(command), panel ? Utf8(panel->mName) : "?");
        return std::nullopt;
      }
    }
  }
  if (command.mVerb == Verb::Profile && command.mProfileStep == 0) {
    for (const auto& profile: mPanels.Profiles()) {
      if (StringUtil::EqualsIgnoreCase(profile.mName, command.mText)) {
        ready.mProfileID = profile.mID;
      }
    }
    if (ready.mProfileID.empty()) {
      why = std::format("{}: there's no profile called that", Automation::Describe(command));
      return std::nullopt;
    }
  }
  return ready;
}

Reply AutomationService::Receive(const COPYDATASTRUCT& data) {
  const auto words = Automation::Unpack(data.dwData, data.lpData, data.cbData);
  if (!words) {
    Refused(std::format("not a command from Open Desktop VR's command line ({} bytes)", data.cbData));
    return Reply::Refused;
  }
  const auto parsed = Automation::Parse(*words);
  if (!parsed.mCommand) {
    Refused(parsed.mError);
    return Reply::Refused;
  }
  if (parsed.mCommand->mVerb == Verb::Help) {
    return Reply::Done;// the command line shows it
  }
  std::string why;
  auto ready = Prepare(*parsed.mCommand, why);
  if (!ready) {
    Refused(why);
    return Reply::Refused;
  }
  if (mQueue.size() >= QueueCapacity) {
    // Never dropped silently, never merged (they're button presses): the
    // sender hears "busy"
    Refused(std::format("{}: {} commands are already waiting", Automation::Describe(ready->mCommand), mQueue.size()));
    return Reply::NoAnswer;
  }
  mQueue.push_back(std::move(*ready));
  // Run after this message returns, from the message loop, as keybinds are
  if (!mRunPosted && PostMessageW(mWindow, RunMessage, 0, 0)) {
    mRunPosted = true;
  }
  return Reply::Done;
}

void AutomationService::RunQueued() {
  mRunPosted = false;
  // Only those here now: one that arrives while these run waits its turn
  for (auto count = mQueue.size(); count > 0 && !mQueue.empty(); --count) {
    const auto ready = std::move(mQueue.front());
    mQueue.pop_front();
    ThreadUtil::CatchAll("command", [&] {
      const auto result = Run(ready);
      Log::Info("Command: {}{}", Automation::Describe(ready.mCommand), result.empty() ? "" : ": " + result);
    });
  }
}

std::string AutomationService::Try(std::span<const std::wstring> words) {
  const auto parsed = Automation::Parse(words);
  if (!parsed.mCommand) {
    return "Not a command: " + parsed.mError;
  }
  if (parsed.mCommand->mVerb == Verb::Help) {
    return "That shows the list of commands.";
  }
  std::string why;
  const auto ready = Prepare(*parsed.mCommand, why);
  if (!ready) {
    return "Refused: " + why;
  }
  const auto result = Run(*ready);
  Log::Info("Command (tried in the settings window): {}{}", Automation::Describe(ready->mCommand), result.empty() ? "" : ": " + result);
  return "Done" + (result.empty() ? std::string {"."} : ": " + result + ".");
}

std::string AutomationService::Run(const Ready& ready) {
  std::string result;
  for (int i = 0; i < std::clamp(ready.mCommand.mRepeat, 1, Automation::MaxRepeat); ++i) {
    result = RunOnce(ready);
  }
  return result;
}

std::string AutomationService::RunOnce(const Ready& ready) {
  const auto& command = ready.mCommand;
  const bool all = command.mTarget.mKind == Target::Kind::All;
  // The panels still here (DU-2: one removed since is skipped)
  std::vector<PanelDefinition> panels;
  for (const auto id: ready.mPanels) {
    if (auto panel = mPanels.Find(id)) {
      panels.push_back(std::move(*panel));
    }
  }
  const auto names = [&] {
    std::string text;
    for (const auto& panel: panels) {
      text += (text.empty() ? "'" : ", '") + Utf8(panel.mName) + "'";
    }
    return text.empty() ? std::string {"no panel (gone)"} : text;
  };
  switch (command.mVerb) {
    case Verb::Show:
    case Verb::Hide: {
      const bool shown = command.mVerb == Verb::Show;
      if (all) {
        return mPanels.SetAllShown(shown) ? (shown ? "all shown" : "all hidden") : "already so";
      }
      for (const auto& panel: panels) {
        mPanels.SetPanelShown(panel.mID, shown);
      }
      return names() + (shown ? " shown" : " hidden");
    }
    case Verb::Toggle:
      if (all) {
        return mPanels.ToggleAll() ? "all shown" : "all hidden";
      }
      for (const auto& panel: panels) {
        mPanels.TogglePanel(panel.mID);
      }
      return names() + " toggled";
    case Verb::Opacity:
    case Verb::Brightness:
    case Verb::Tint: {
      for (auto panel: panels) {
        const auto level = [&](float current) {
          const float percent = command.mRelative ? (current * 100.0f) + command.mPercent : command.mPercent;
          return std::clamp(std::round(percent) / 100.0f, PanelDefinition::MinimumLevel, 1.0f);
        };
        if (command.mVerb == Verb::Opacity) {
          const float old = panel.mOpacity;
          panel.mOpacity = level(old);
          if (panel.mFocusedOpacity == old) {
            panel.mFocusedOpacity = panel.mOpacity;// they went together: so they stay (as the quick menu does)
          }
        } else if (command.mVerb == Verb::Brightness) {
          panel.mBrightness = level(panel.mBrightness);
        } else {
          panel.mTintEnabled = command.mSwitch == Switch::On ? true
            : command.mSwitch == Switch::Off                 ? false
                                                             : !panel.mTintEnabled;
        }
        mPanels.Update(panel);
      }
      return names();
    }
    case Verb::Nudge:
      return Nudge(ready);
    case Verb::Window:
      if (mWindowRequests.size() >= MaxWindowRequests) {
        return "skipped: too many window changes waiting";
      }
      mWindowRequests.push_back({ready.mPanels, command.mText});
      StartListing();
      return "finding the window";
    case Verb::Recenter:
      mRenderer.Recenter();
      return {};
    case Verb::Profile:
      if (command.mProfileStep != 0) {
        return mPanels.StepProfile(command.mProfileStep) ? Utf8(mPanels.ProfileName(mPanels.ActiveProfile()))
                                                          : "there's no other profile";
      }
      return mPanels.SwitchProfile(ready.mProfileID, "a command") ? "switched" : "already the profile";
    case Verb::Keyboard:
      if (command.mSwitch == Switch::Off) {
        mKeyboard.Dismiss("a command");
      } else if (command.mSwitch == Switch::Toggle || !mKeyboard.IsShown()) {
        mKeyboard.Toggle();
      }
      return mKeyboard.IsShown() ? "showing" : "hidden";
    case Verb::Menu:
      if (command.mSwitch == Switch::On) {
        mQuickMenu.Open("a command");
      } else if (command.mSwitch == Switch::Off) {
        mQuickMenu.Close("a command");
      } else {
        mQuickMenu.Toggle("a command");
      }
      return mQuickMenu.IsOpen() ? "open" : "closed";
    case Verb::Settings:
      if (mOpenSettings) {
        mOpenSettings();
      }
      return {};
    case Verb::Help:
      return {};
  }
  return {};
}

std::string AutomationService::Nudge(const Ready& ready) {
  using Automation::Direction;
  using PanelPlacementConfig::Nudge;
  const auto& command = ready.mCommand;
  const float cm = command.mAmount / 100.0f;
  const float degrees = command.mAmount;
  const auto move = [&](PanelPlacement& placement) {
    switch (command.mDirection) {
      case Direction::Left:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Across, -cm);
        break;
      case Direction::Right:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Across, cm);
        break;
      case Direction::Up:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Up, cm);
        break;
      case Direction::Down:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Up, -cm);
        break;
      case Direction::Closer:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Away, -cm);
        break;
      case Direction::Farther:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Away, cm);
        break;
      case Direction::TurnLeft:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Turn, degrees);// positive turns it to its left
        break;
      case Direction::TurnRight:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Turn, -degrees);
        break;
      case Direction::TiltBack:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Tilt, degrees);// its top away
        break;
      case Direction::TiltForward:
        PanelPlacementConfig::NudgeBy(placement, Nudge::Tilt, -degrees);
        break;
      case Direction::Bigger:
      case Direction::Smaller:
        break;// sizes below
    }
  };
  const bool resize = command.mDirection == Direction::Bigger || command.mDirection == Direction::Smaller;
  const float factor = command.mDirection == Direction::Bigger ? 1.0f + (command.mAmount / 100.0f) : 1.0f - (command.mAmount / 100.0f);
  if (command.mTarget.mKind == Target::Kind::Keyboard) {
    auto keyboard = mPanels.Keyboard();
    if (resize) {
      keyboard.mKeyMillimeters = std::clamp(
        std::round(keyboard.mKeyMillimeters * factor * 10.0f) / 10.0f, KeyboardSettings::MinKey, KeyboardSettings::MaxKey);
    } else {
      move(keyboard.mPlacement);
    }
    mPanels.SetKeyboard(keyboard);
    return resize ? std::format("keys {:.1f} mm apart", keyboard.mKeyMillimeters) : std::string {};
  }
  std::string text;
  for (const auto id: ready.mPanels) {
    auto panel = mPanels.Find(id);
    if (!panel) {
      continue;// DU-2
    }
    if (resize) {
      // The settings window's limits: 5 cm to 5 m; a box keeps its shape
      const float width = std::clamp(panel->mWidthMeters * factor, 0.05f, 5.0f);
      const float applied = width / std::max(panel->mWidthMeters, 0.001f);
      panel->mWidthMeters = width;
      if (panel->mMaxHeightMeters > 0) {
        panel->mMaxHeightMeters = std::clamp(panel->mMaxHeightMeters * applied, 0.05f, 5.0f);
      }
    } else {
      auto placement = PanelPlacementConfig::From(*panel);
      move(placement);
      PanelPlacementConfig::ApplyTo(placement, *panel);
    }
    mPanels.Update(*panel);
    text += (text.empty() ? "'" : ", '") + Utf8(panel->mName) + "'";
  }
  return text.empty() ? "no panel (gone)" : text;
}

void AutomationService::StartListing() {
  if (mListing.Running()) {
    return;// the list on its way serves this request too
  }
  try {
    mListing = ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>>::Start(
      L"Command windows", [] { return WindowUtil::ListCaptureCandidates(); });
  } catch (const std::exception& e) {
    Log::Warning("Commands: couldn't list windows: {}", e.what());
    mWindowRequests.clear();
    return;
  }
  SetTimer(mWindow, ListingTimer, ListingCheckMs, nullptr);
}

void AutomationService::CheckListing() {
  if (mListing.Running()) {
    return;
  }
  KillTimer(mWindow, ListingTimer);
  const auto windows = mListing.Take().value_or(std::vector<WindowUtil::WindowInfo> {});
  for (const auto& request: std::exchange(mWindowRequests, {})) {
    const auto found = std::ranges::find_if(
      windows, [&](const WindowUtil::WindowInfo& window) { return StringUtil::ContainsIgnoreCase(window.mTitle, request.mText); });
    if (found == windows.end()) {
      Log::Info("Command: window '{}': no window's title has that in it", Utf8(request.mText));
      continue;
    }
    for (const auto id: request.mPanels) {
      auto panel = mPanels.Find(id);
      if (!panel || panel->mKind != PanelKind::Window) {
        continue;// DU-2
      }
      panel->mWindow = WindowMatch::FromWindow(*found);
      mPanels.Update(*panel);
      Log::Info("Command: '{}' now shows '{}'", Utf8(panel->mName), Utf8(found->mTitle));
    }
  }
}

LRESULT CALLBACK AutomationService::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
  }
  auto* self = reinterpret_cast<AutomationService*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self && message == WM_COPYDATA && lParam) {
    // Another process's words (TH-9): checked, never trusted, never thrown
    // out of here
    auto reply = Reply::Refused;
    ThreadUtil::CatchAll("command received", [&] { reply = self->Receive(*reinterpret_cast<const COPYDATASTRUCT*>(lParam)); });
    return ReplyBase + static_cast<LRESULT>(reply);
  }
  if (self && message == RunMessage) {
    ThreadUtil::CatchAll("commands", [&] { self->RunQueued(); });
    return 0;
  }
  if (self && message == WM_TIMER && wParam == ListingTimer) {
    ThreadUtil::CatchAll("command windows", [&] { self->CheckListing(); });
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}// namespace OpenDesktopVR::Host
