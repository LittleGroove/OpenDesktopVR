// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Automation.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <format>

namespace OpenDesktopVR::Automation {

namespace {

bool Is(const std::wstring& word, std::wstring_view name) {
  return StringUtil::EqualsIgnoreCase(word, std::wstring {name});
}

// A word for an error message: shortened, and safe to log
std::string Quote(const std::wstring& word) {
  auto text = StringUtil::Narrow(word.size() > 40 ? word.substr(0, 40) + L"..." : word);
  return "'" + text + "'";
}

// "50", "50%", "+10", "-10.5%": the number, and whether it had a sign
std::optional<float> Number(std::wstring text, bool* signedOut = nullptr) {
  if (!text.empty() && text.back() == L'%') {
    text.pop_back();
  }
  if (text.empty() || text.size() > 12) {
    return std::nullopt;
  }
  const bool sign = text[0] == L'+' || text[0] == L'-';
  for (std::size_t i = sign ? 1 : 0; i < text.size(); ++i) {
    if (!std::iswdigit(text[i]) && text[i] != L'.') {
      return std::nullopt;
    }
  }
  wchar_t* end = nullptr;
  const double value = std::wcstod(text.c_str(), &end);
  if (!end || *end != L'\0' || !std::isfinite(value)) {
    return std::nullopt;
  }
  if (signedOut) {
    *signedOut = sign;
  }
  return static_cast<float>(value);
}

std::optional<int> WholeNumber(const std::wstring& text) {
  if (text.empty() || text.size() > 4 || !std::ranges::all_of(text, [](wchar_t c) { return std::iswdigit(c) != 0; })) {
    return std::nullopt;
  }
  return std::stoi(text);
}

struct DirectionInfo {
  const wchar_t* mWord;
  Direction mDirection;
  float mDefault;// how far without an amount
  float mMaximum;
  const char* mUnit;
};
constexpr DirectionInfo Directions[] {
  {L"left", Direction::Left, 1, 100, "cm"},
  {L"right", Direction::Right, 1, 100, "cm"},
  {L"up", Direction::Up, 1, 100, "cm"},
  {L"down", Direction::Down, 1, 100, "cm"},
  {L"closer", Direction::Closer, 1, 100, "cm"},
  {L"farther", Direction::Farther, 1, 100, "cm"},
  {L"bigger", Direction::Bigger, 5, 100, "%"},
  {L"smaller", Direction::Smaller, 5, 90, "%"},
  {L"turn-left", Direction::TurnLeft, 1, 90, "degrees"},
  {L"turn-right", Direction::TurnRight, 1, 90, "degrees"},
  {L"tilt-back", Direction::TiltBack, 1, 90, "degrees"},
  {L"tilt-forward", Direction::TiltForward, 1, 90, "degrees"},
};

const DirectionInfo* FindDirection(Direction direction) {
  for (const auto& info: Directions) {
    if (info.mDirection == direction) {
      return &info;
    }
  }
  return nullptr;
}

Target PanelOrAll(const std::wstring& word, bool keyboard) {
  if (Is(word, L"all") || word == L"*") {
    return {Target::Kind::All, {}};
  }
  if (keyboard && Is(word, L"keyboard")) {
    return {Target::Kind::Keyboard, {}};
  }
  return {Target::Kind::Panel, word};
}

std::optional<Switch> SwitchWord(const std::wstring& word, std::wstring_view on, std::wstring_view off) {
  if (Is(word, on)) {
    return Switch::On;
  }
  if (Is(word, off)) {
    return Switch::Off;
  }
  if (Is(word, L"toggle")) {
    return Switch::Toggle;
  }
  return std::nullopt;
}

std::wstring Join(std::span<const std::wstring> words) {
  std::wstring text;
  for (const auto& word: words) {
    text += (text.empty() ? L"" : L" ") + word;
  }
  return text;
}

Parsed Error(std::string why) {
  return {std::nullopt, std::move(why)};
}

Parsed ParseWords(std::span<const std::wstring> words) {
  if (words.size() > MaxWords) {
    return Error(std::format("too many words ({}; at most {})", words.size(), MaxWords));
  }
  std::vector<std::wstring> rest;
  int repeat = 1;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const auto& word = words[i];
    if (word.size() > MaxWordLength) {
      return Error(std::format("a word is too long ({} characters; at most {})", word.size(), MaxWordLength));
    }
    if (std::ranges::any_of(word, [](wchar_t c) { return c < 0x20 || c == 0x7F; })) {
      return Error("a word has control characters in it");
    }
    if (Is(word, L"--repeat")) {
      const auto count = i + 1 < words.size() ? WholeNumber(words[i + 1]) : std::nullopt;
      if (!count || *count < 1 || *count > MaxRepeat) {
        return Error(std::format("--repeat needs a count from 1 to {}", MaxRepeat));
      }
      repeat = *count;
      ++i;
      continue;
    }
    if (word.starts_with(L"--")) {
      return Error("unknown option " + Quote(word));
    }
    rest.push_back(word);
  }
  if (rest.empty()) {
    return Error("no command (\"help\" lists them)");
  }
  const auto& verb = rest[0];
  const std::span<const std::wstring> args {rest.begin() + 1, rest.end()};
  Command command;
  command.mRepeat = repeat;
  const auto tooMany = [&](std::size_t allowed) { return args.size() > allowed; };

  if (Is(verb, L"show") || Is(verb, L"hide") || Is(verb, L"toggle")) {
    command.mVerb = Is(verb, L"show") ? Verb::Show : Is(verb, L"hide") ? Verb::Hide : Verb::Toggle;
    if (args.empty()) {
      return Error(StringUtil::Narrow(verb) + " needs a panel, or all");
    }
    if (tooMany(1)) {
      return Error("too many words after " + StringUtil::Narrow(verb) + " (put a name with spaces in quotes)");
    }
    command.mTarget = PanelOrAll(args[0], false);
    return {command, {}};
  }
  if (Is(verb, L"opacity") || Is(verb, L"brightness")) {
    command.mVerb = Is(verb, L"opacity") ? Verb::Opacity : Verb::Brightness;
    if (args.size() != 2) {
      return Error(StringUtil::Narrow(verb) + " needs a panel (or all) and a percentage: 50, +10 or -10");
    }
    command.mTarget = PanelOrAll(args[0], false);
    bool relative = false;
    const auto value = Number(args[1], &relative);
    if (!value || std::abs(*value) > 100 || (!relative && *value < 0)) {
      return Error("not a percentage: " + Quote(args[1]) + " (50, +10 or -10)");
    }
    command.mPercent = *value;
    command.mRelative = relative;
    return {command, {}};
  }
  if (Is(verb, L"tint")) {
    command.mVerb = Verb::Tint;
    if (args.empty() || tooMany(2)) {
      return Error("tint needs a panel (or all), then on, off or toggle");
    }
    command.mTarget = PanelOrAll(args[0], false);
    if (args.size() == 2) {
      const auto which = SwitchWord(args[1], L"on", L"off");
      if (!which) {
        return Error("tint: " + Quote(args[1]) + " isn't on, off or toggle");
      }
      command.mSwitch = *which;
    }
    return {command, {}};
  }
  if (Is(verb, L"nudge")) {
    command.mVerb = Verb::Nudge;
    if (args.size() < 2 || tooMany(3)) {
      return Error("nudge needs a panel (or all, or keyboard), a direction and maybe an amount: nudge Discord left 2");
    }
    command.mTarget = PanelOrAll(args[0], true);
    const DirectionInfo* found = nullptr;
    for (const auto& info: Directions) {
      if (Is(args[1], info.mWord)) {
        found = &info;
      }
    }
    if (!found) {
      return Error("nudge: " + Quote(args[1])
        + " isn't a direction (left, right, up, down, closer, farther, bigger, smaller, turn-left, turn-right, tilt-back, "
          "tilt-forward)");
    }
    command.mDirection = found->mDirection;
    command.mAmount = found->mDefault;
    if (args.size() == 3) {
      bool sign = false;
      const auto amount = Number(args[2], &sign);
      if (!amount || sign || *amount <= 0 || *amount > found->mMaximum) {
        return Error(std::format("nudge: {} isn't an amount from 0 to {} {}", Quote(args[2]), found->mMaximum, found->mUnit));
      }
      command.mAmount = *amount;
    }
    return {command, {}};
  }
  if (Is(verb, L"window")) {
    command.mVerb = Verb::Window;
    if (args.size() < 2) {
      return Error("window needs a panel, then text from the window's title: window Video YouTube");
    }
    command.mTarget = PanelOrAll(args[0], false);
    if (command.mTarget.mKind != Target::Kind::Panel) {
      return Error("window needs one panel, not all");
    }
    command.mText = Join(args.subspan(1));
    return {command, {}};
  }
  if (Is(verb, L"recenter")) {
    command.mVerb = Verb::Recenter;
    if (tooMany(0)) {
      return Error("recenter takes nothing after it");
    }
    return {command, {}};
  }
  if (Is(verb, L"profile")) {
    command.mVerb = Verb::Profile;
    if (args.empty()) {
      return Error("profile needs a name, next or previous");
    }
    if (args.size() == 1 && Is(args[0], L"next")) {
      command.mProfileStep = 1;
    } else if (args.size() == 1 && (Is(args[0], L"previous") || Is(args[0], L"prev"))) {
      command.mProfileStep = -1;
    } else {
      command.mText = Join(args);
    }
    return {command, {}};
  }
  if (Is(verb, L"keyboard") || Is(verb, L"menu")) {
    const bool keyboard = Is(verb, L"keyboard");
    command.mVerb = keyboard ? Verb::Keyboard : Verb::Menu;
    if (tooMany(1)) {
      return Error(StringUtil::Narrow(verb) + (keyboard ? ": show, hide or toggle" : ": open, close or toggle"));
    }
    if (args.size() == 1) {
      const auto which = keyboard ? SwitchWord(args[0], L"show", L"hide") : SwitchWord(args[0], L"open", L"close");
      if (!which) {
        return Error(StringUtil::Narrow(verb) + ": " + Quote(args[0])
          + (keyboard ? " isn't show, hide or toggle" : " isn't open, close or toggle"));
      }
      command.mSwitch = *which;
    }
    return {command, {}};
  }
  if (Is(verb, L"help") || verb == L"/?") {
    command.mVerb = Verb::Help;
    return {command, {}};
  }
  return Error("unknown command " + Quote(verb) + " (\"help\" lists them)");
}

}// namespace

Parsed Parse(std::span<const std::wstring> words) noexcept {
  try {
    return ParseWords(words);
  } catch (...) {
    return {std::nullopt, "couldn't read the command"};
  }
}

bool IsCommand(std::span<const std::wstring> words) noexcept {
  return !words.empty() && !words[0].starts_with(L"-");
}

std::vector<uint64_t> Resolve(const std::wstring& selector, std::span<const PanelName> panels) {
  std::vector<uint64_t> ids;
  for (const auto& panel: panels) {
    if (StringUtil::EqualsIgnoreCase(panel.mName, selector)) {
      ids.push_back(panel.mID);
    }
  }
  if (!ids.empty() || selector.empty()) {
    return ids;
  }
  if (selector[0] == L'#') {
    if (const auto position = WholeNumber(selector.substr(1)); position && *position >= 1
        && static_cast<std::size_t>(*position) <= panels.size()) {
      ids.push_back(panels[static_cast<std::size_t>(*position) - 1].mID);
    }
    return ids;
  }
  if (selector.size() == 16 && std::ranges::all_of(selector, [](wchar_t c) { return std::iswxdigit(c) != 0; })) {
    const uint64_t id = std::wcstoull(selector.c_str(), nullptr, 16);
    for (const auto& panel: panels) {
      if (panel.mID == id) {
        ids.push_back(id);
      }
    }
  }
  return ids;
}

std::vector<wchar_t> Pack(std::span<const std::wstring> words) {
  // At most one word and one character past the limits: enough for the
  // host to say what was wrong, never more than it accepts
  std::vector<wchar_t> packed;
  for (std::size_t i = 0; i < words.size() && i <= MaxWords; ++i) {
    const auto& word = words[i];
    packed.insert(packed.end(), word.begin(), word.begin() + static_cast<std::ptrdiff_t>(std::min(word.size(), MaxWordLength + 1)));
    packed.push_back(L'\0');
  }
  return packed;
}

std::optional<std::vector<std::wstring>> Unpack(uintptr_t tag, const void* data, std::size_t bytes) noexcept {
  try {
    if (tag != WireTag || !data || bytes < sizeof(wchar_t) || bytes > MaxWireBytes || bytes % sizeof(wchar_t) != 0) {
      return std::nullopt;
    }
    const auto* chars = static_cast<const wchar_t*>(data);
    const std::size_t count = bytes / sizeof(wchar_t);
    if (chars[count - 1] != L'\0') {
      return std::nullopt;
    }
    std::vector<std::wstring> words;
    std::size_t start = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (chars[i] == L'\0') {
        words.emplace_back(chars + start, chars + i);
        start = i + 1;
      }
    }
    return words;
  } catch (...) {
    return std::nullopt;
  }
}

std::wstring Usage() {
  return LR"(Open Desktop VR commands, for Stream Deck, VoiceAttack and other tools:

  OpenDesktopVR.exe show <panel>          (or hide, or toggle; <panel> can be all)
  OpenDesktopVR.exe opacity <panel> 50    (or +10, -10; brightness the same)
  OpenDesktopVR.exe tint <panel> on       (or off, toggle)
  OpenDesktopVR.exe nudge <panel> left 2  (left, right, up, down, closer, farther: cm;
                                           bigger, smaller: %; turn-left, turn-right,
                                           tilt-back, tilt-forward: degrees)
  OpenDesktopVR.exe window <panel> <text in the window's title>
  OpenDesktopVR.exe recenter
  OpenDesktopVR.exe profile <name>        (or next, previous)
  OpenDesktopVR.exe keyboard toggle       (or show, hide)
  OpenDesktopVR.exe menu toggle           (or open, close)

A panel is its name ("Discord"; quotes if it has spaces), its place in the
list ("#2"), or all. nudge also moves the keyboard. Add --repeat 3 to do it
three times. The settings window's Keybinds tab builds these for you.)";
}

std::string Describe(const Command& command) {
  const auto target = [&]() -> std::string {
    switch (command.mTarget.mKind) {
      case Target::Kind::All:
        return "all panels";
      case Target::Kind::Keyboard:
        return "the keyboard";
      case Target::Kind::Panel:
        return Quote(command.mTarget.mSelector);
      case Target::Kind::None:
        break;
    }
    return {};
  };
  const auto onOff = [&](const char* on, const char* off) {
    return command.mSwitch == Switch::On ? on : command.mSwitch == Switch::Off ? off : "toggle";
  };
  std::string text;
  switch (command.mVerb) {
    case Verb::Show:
      text = "show " + target();
      break;
    case Verb::Hide:
      text = "hide " + target();
      break;
    case Verb::Toggle:
      text = "toggle " + target();
      break;
    case Verb::Opacity:
    case Verb::Brightness:
      text = std::format("{} {} {}{:g}%", command.mVerb == Verb::Opacity ? "opacity" : "brightness", target(),
        command.mRelative && command.mPercent >= 0 ? "+" : "", command.mPercent);
      break;
    case Verb::Tint:
      text = std::format("tint {} {}", target(), onOff("on", "off"));
      break;
    case Verb::Nudge: {
      const auto* info = FindDirection(command.mDirection);
      text = std::format("nudge {} {} {:g} {}", target(), info ? StringUtil::Narrow(info->mWord) : "?", command.mAmount,
        info ? info->mUnit : "");
      break;
    }
    case Verb::Window:
      text = std::format("window {} {}", target(), Quote(command.mText));
      break;
    case Verb::Recenter:
      text = "recenter";
      break;
    case Verb::Profile:
      text = command.mProfileStep > 0 ? "profile next"
        : command.mProfileStep < 0    ? "profile previous"
                                      : "profile " + Quote(command.mText);
      break;
    case Verb::Keyboard:
      text = std::format("keyboard {}", onOff("show", "hide"));
      break;
    case Verb::Menu:
      text = std::format("menu {}", onOff("open", "close"));
      break;
    case Verb::Help:
      text = "help";
      break;
  }
  return command.mRepeat > 1 ? std::format("{} (x{})", text, command.mRepeat) : text;
}

}// namespace OpenDesktopVR::Automation
