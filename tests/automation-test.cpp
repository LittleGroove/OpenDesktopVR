// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Automation (M6e, UI-9, ADR-042), without a host:
// - every command parses into what it says, with its defaults and limits;
// - nonsense is refused with a reason, never guessed at (TH-9): unknown
//   words, bad numbers, too many or too long words, control characters;
// - only a first word that isn't an option makes a command;
// - panels by name (without case; every panel with it), "#N", or ID;
// - the words packed for WM_COPYDATA and unpacked, and anything else
//   refused: the wrong tag, odd or huge sizes, no final NUL;
// - fuzz: random words and random bytes never crash or throw (doc 11:
//   "malformed external commands: logged and dropped; no crash");
// - nudges snap, so steps there and back end where they started (test
//   round 20's "-0.0 cm").

#include "check.hpp"

#include <OpenDesktopVR/Automation.hpp>
#include <OpenDesktopVR/PanelPlacement.hpp>

#include <cmath>
#include <format>
#include <random>
#include <string>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Automation;
using Testing::Check;
using Testing::Scenario;

namespace {

Parsed P(std::vector<std::wstring> words) {
  return Parse(words);
}

bool Refused(std::vector<std::wstring> words, const char* expect = nullptr) {
  const auto parsed = Parse(words);
  return !parsed.mCommand && !parsed.mError.empty() && (!expect || parsed.mError.find(expect) != std::string::npos);
}

}// namespace

int main() {
  Scenario("Commands parse into what they say", [] {
    auto c = P({L"toggle", L"Discord"}).mCommand;
    Check(c && c->mVerb == Verb::Toggle && c->mTarget.mKind == Target::Kind::Panel && c->mTarget.mSelector == L"Discord"
        && c->mRepeat == 1,
      "toggle Discord");
    c = P({L"HIDE", L"all"}).mCommand;
    Check(c && c->mVerb == Verb::Hide && c->mTarget.mKind == Target::Kind::All, "HIDE all (words without case)");
    c = P({L"show", L"*"}).mCommand;
    Check(c && c->mTarget.mKind == Target::Kind::All, "show * (all)");
    c = P({L"opacity", L"#2", L"+10"}).mCommand;
    Check(c && c->mVerb == Verb::Opacity && c->mRelative && c->mPercent == 10 && c->mTarget.mSelector == L"#2", "opacity #2 +10");
    c = P({L"brightness", L"all", L"50%"}).mCommand;
    Check(c && c->mVerb == Verb::Brightness && !c->mRelative && c->mPercent == 50, "brightness all 50%");
    c = P({L"opacity", L"Video", L"-25"}).mCommand;
    Check(c && c->mRelative && c->mPercent == -25, "opacity Video -25");
    c = P({L"tint", L"Video"}).mCommand;
    Check(c && c->mVerb == Verb::Tint && c->mSwitch == Switch::Toggle, "tint Video: toggles");
    c = P({L"tint", L"Video", L"on"}).mCommand;
    Check(c && c->mSwitch == Switch::On, "tint Video on");
    c = P({L"nudge", L"Discord", L"left"}).mCommand;
    Check(c && c->mVerb == Verb::Nudge && c->mDirection == Direction::Left && c->mAmount == 1, "nudge Discord left: 1 cm");
    c = P({L"nudge", L"keyboard", L"bigger"}).mCommand;
    Check(c && c->mTarget.mKind == Target::Kind::Keyboard && c->mDirection == Direction::Bigger && c->mAmount == 5,
      "nudge keyboard bigger: 5%");
    c = P({L"nudge", L"#1", L"turn-right", L"2.5", L"--repeat", L"4"}).mCommand;
    Check(c && c->mDirection == Direction::TurnRight && c->mAmount == 2.5f && c->mRepeat == 4, "nudge #1 turn-right 2.5 --repeat 4");
    c = P({L"--repeat", L"2", L"recenter"}).mCommand;
    Check(c && c->mVerb == Verb::Recenter && c->mRepeat == 2, "--repeat before the verb works too");
    c = P({L"window", L"Video", L"YouTube", L"-", L"Firefox"}).mCommand;
    Check(c && c->mVerb == Verb::Window && c->mText == L"YouTube - Firefox", "window Video YouTube - Firefox (the rest is the title)");
    c = P({L"profile", L"next"}).mCommand;
    Check(c && c->mVerb == Verb::Profile && c->mProfileStep == 1, "profile next");
    c = P({L"profile", L"prev"}).mCommand;
    Check(c && c->mProfileStep == -1, "profile prev");
    c = P({L"profile", L"Elite", L"Dangerous"}).mCommand;
    Check(c && c->mProfileStep == 0 && c->mText == L"Elite Dangerous", "profile Elite Dangerous (unquoted)");
    c = P({L"keyboard"}).mCommand;
    Check(c && c->mVerb == Verb::Keyboard && c->mSwitch == Switch::Toggle, "keyboard: toggles");
    c = P({L"keyboard", L"hide"}).mCommand;
    Check(c && c->mSwitch == Switch::Off, "keyboard hide");
    c = P({L"menu", L"open"}).mCommand;
    Check(c && c->mVerb == Verb::Menu && c->mSwitch == Switch::On, "menu open");
    c = P({L"settings"}).mCommand;
    Check(c && c->mVerb == Verb::Settings && Refused({L"settings", L"now"}), "settings (what starting it again sends)");
    c = P({L"help"}).mCommand;
    Check(c && c->mVerb == Verb::Help, "help");
    c = P({L"page", L"Checklist", L"next"}).mCommand;
    Check(c && c->mVerb == Verb::Page && c->mPageStep == 1 && c->mTarget.mSelector == L"Checklist", "page Checklist next");
    c = P({L"PAGE", L"all", L"prev"}).mCommand;
    Check(c && c->mPageStep == -1 && c->mTarget.mKind == Target::Kind::All, "PAGE all prev");
    c = P({L"page", L"#2", L"12"}).mCommand;
    Check(c && c->mPageStep == 0 && c->mPage == 12, "page #2 12");
    c = P({L"page", L"Charts", L"first"}).mCommand;
    Check(c && c->mPage == 1, "page Charts first");
    c = P({L"page", L"Charts", L"last", L"--repeat", L"2"}).mCommand;
    Check(c && c->mPage == UINT32_MAX && c->mRepeat == 2, "page Charts last");
    Check(Describe(*P({L"page", L"Charts", L"next"}).mCommand) == "page 'Charts' next"
        && Describe(*P({L"page", L"Charts", L"7"}).mCommand) == "page 'Charts' 7",
      "described for the log");
  });

  Scenario("Nonsense is refused with a reason (TH-9)", [] {
    Check(Refused({L"toogle", L"all"}, "unknown command"), "an unknown command");
    Check(Refused({}, "no command") && Refused({L"--repeat", L"2"}, "no command"), "no command at all");
    Check(Refused({L"toggle"}, "needs a panel"), "toggle without a panel");
    Check(Refused({L"toggle", L"My", L"Panel"}, "quotes"), "an unquoted name with a space: say to quote it");
    Check(Refused({L"opacity", L"#1", L"lots"}, "percentage") && Refused({L"opacity", L"#1", L"150"})
        && Refused({L"opacity", L"#1", L"-"}) && Refused({L"opacity", L"#1", L"1e3"}) && Refused({L"opacity", L"#1"}),
      "opacity: not a number, over 100, a bare sign, an exponent, nothing");
    Check(Refused({L"nudge", L"#1", L"sideways"}, "direction") && Refused({L"nudge", L"#1", L"left", L"500"})
        && Refused({L"nudge", L"#1", L"left", L"-2"}) && Refused({L"nudge", L"#1", L"left", L"0"}),
      "nudge: an unknown direction, too far, negative, zero");
    Check(Refused({L"tint", L"#1", L"maybe"}) && Refused({L"keyboard", L"open"}) && Refused({L"menu", L"show"}),
      "switches: only their own words");
    Check(Refused({L"window", L"all", L"x"}) && Refused({L"window", L"#1"}), "window: one panel, and some title text");
    Check(Refused({L"recenter", L"now"}), "recenter takes nothing after it");
    Check(Refused({L"page", L"#1"}, "page needs") && Refused({L"page", L"#1", L"0"}) && Refused({L"page", L"#1", L"-2"})
        && Refused({L"page", L"#1", L"+2"}) && Refused({L"page", L"#1", L"2.5"}) && Refused({L"page", L"#1", L"soon"})
        && Refused({L"page", L"#1", L"next", L"again"}) && Refused({L"page", L"#1", L"9999999"}),
      "page: nothing, zero, signed, a fraction, a word, too many, too far");
    Check(Refused({L"toggle", L"all", L"--repeat", L"0"}) && Refused({L"toggle", L"all", L"--repeat", L"21"})
        && Refused({L"toggle", L"all", L"--repeat"}) && Refused({L"toggle", L"all", L"--force"}),
      "--repeat out of 1 to 20, missing, and unknown options");
    std::vector<std::wstring> many(MaxWords + 1, L"x");
    Check(Refused(many, "too many words"), "too many words");
    Check(Refused({L"toggle", std::wstring(MaxWordLength + 1, L'a')}, "too long"), "a word too long");
    Check(Refused({L"toggle", std::wstring {L"Dis\x0007" L"cord"}}, "control"), "a control character");
  });

  Scenario("Only a first word that isn't an option makes a command", [] {
    const std::vector<std::wstring> none;
    Check(!IsCommand(none), "no words: the host starts");
    const std::vector<std::wstring> option {L"--settings"};
    Check(!IsCommand(option), "--settings: the host's own option");
    const std::vector<std::wstring> window {L"--window", L"Click Target"};
    Check(!IsCommand(window), "--window \"Click Target\": an option's value isn't a command");
    const std::vector<std::wstring> verb {L"toggle", L"all"};
    Check(IsCommand(verb), "toggle all: a command");
  });

  Scenario("Panels by name, place or ID", [] {
    const std::vector<PanelName> panels {{0x1111, L"Discord"}, {0x2222, L"Video"}, {0x3333, L"discord"}, {0x0D0E0001, L"Map"}};
    Check(Resolve(L"DISCORD", panels) == std::vector<uint64_t> {0x1111, 0x3333}, "a name without case: every panel with it");
    Check(Resolve(L"#2", panels) == std::vector<uint64_t> {0x2222}, "#2: the second in the list");
    Check(Resolve(L"#0", panels).empty() && Resolve(L"#5", panels).empty() && Resolve(L"#x", panels).empty(),
      "#0, #5 (of 4) and #x: none");
    Check(Resolve(L"000000000D0E0001", panels) == std::vector<uint64_t> {0x0D0E0001}, "a 16-digit ID");
    Check(Resolve(L"00000000DEADBEEF", panels).empty() && Resolve(L"Spotify", panels).empty() && Resolve(L"", panels).empty(),
      "an unknown ID, an unknown name, nothing: none");
  });

  Scenario("The words packed for WM_COPYDATA, and anything else refused", [] {
    const std::vector<std::wstring> words {L"nudge", L"Elite Dangerous", L"left", L"2"};
    auto packed = Pack(words);
    const auto back = Unpack(WireTag, packed.data(), packed.size() * sizeof(wchar_t));
    Check(back && *back == words, "packed and unpacked: the same words");
    Check(!Unpack(WireTag + 1, packed.data(), packed.size() * sizeof(wchar_t)), "the wrong tag");
    Check(!Unpack(WireTag, packed.data(), (packed.size() * sizeof(wchar_t)) - 1), "an odd size");
    Check(!Unpack(WireTag, packed.data(), (packed.size() - 1) * sizeof(wchar_t)), "no final NUL");
    Check(!Unpack(WireTag, nullptr, 8) && !Unpack(WireTag, packed.data(), 0), "no data");
    std::vector<wchar_t> huge(MaxWireBytes / sizeof(wchar_t) + 2, L'a');
    huge.back() = L'\0';
    Check(!Unpack(WireTag, huge.data(), huge.size() * sizeof(wchar_t)), "more than the most there could be");
    // Pack keeps just enough past the limits for the host to say which
    std::vector<std::wstring> tooMany(40, L"x");
    tooMany[0] = std::wstring(1000, L'b');
    const auto bounded = Pack(tooMany);
    const auto unpacked = Unpack(WireTag, bounded.data(), bounded.size() * sizeof(wchar_t));
    Check(unpacked && unpacked->size() == MaxWords + 1 && (*unpacked)[0].size() == MaxWordLength + 1 && !Parse(*unpacked).mCommand,
      "too many, too long: packed within bounds, and still refused there");
  });

  Scenario("Fuzz: random words and bytes never crash or throw", [] {
    std::mt19937 random {20261118};
    const std::vector<std::wstring> vocabulary {L"show", L"hide", L"toggle", L"opacity", L"brightness", L"tint", L"nudge",
      L"window", L"recenter", L"profile", L"keyboard", L"menu", L"help", L"all", L"*", L"#1", L"#99999", L"left", L"bigger",
      L"turn-left", L"on", L"off", L"--repeat", L"--x", L"+10", L"-10", L"10%", L"1e9", L"nan", L"inf", L"", L"\x0001",
      L"0000000000000001", L"keyboard", L"next", L"\xFFFF", L"\xD800"};
    int commands = 0;
    int refused = 0;
    for (int round = 0; round < 20'000; ++round) {
      std::vector<std::wstring> words(random() % 20);
      for (auto& word: words) {
        if (random() % 4 == 0) {
          word.resize(random() % 300);
          for (auto& c: word) {
            c = static_cast<wchar_t>(random() % 0x10000);
          }
        } else {
          word = vocabulary[random() % vocabulary.size()];
        }
      }
      const auto parsed = Parse(words);
      (parsed.mCommand ? commands : refused) += 1;
      if (parsed.mCommand) {
        (void)Describe(*parsed.mCommand);
      }
    }
    std::vector<uint8_t> bytes;
    int unpacked = 0;
    for (int round = 0; round < 5'000; ++round) {
      bytes.resize(random() % 4096);
      for (auto& b: bytes) {
        b = static_cast<uint8_t>(random());
      }
      if (random() % 2 == 0 && bytes.size() >= 2) {
        bytes[bytes.size() - 1] = 0;
        bytes[bytes.size() - 2] = 0;
      }
      if (const auto words = Unpack(WireTag, bytes.data(), bytes.size())) {
        ++unpacked;
        (void)Parse(*words);
      }
    }
    Check(commands > 0 && refused > 0, std::format("20,000 random commands: {} read, {} refused, none threw", commands, refused));
    Check(true, std::format("5,000 random messages: {} unpacked (then parsed), none crashed", unpacked));
  });

  Scenario("Help and logs", [] {
    Check(Usage().find(L"nudge") != std::wstring::npos && Usage().find(L"--repeat") != std::wstring::npos, "help lists the commands");
    const auto c = P({L"nudge", L"#2", L"left", L"2", L"--repeat", L"3"}).mCommand;
    Check(c && Describe(*c) == "nudge '#2' left 2 cm (x3)", c ? "logged as: " + Describe(*c) : "parsed");
  });

  Scenario("Nudges snap: there and back ends where it started (test round 20)", [] {
    using namespace Host::PanelPlacementConfig;
    Host::PanelPlacement p;
    p.mPosition = {0, -0.3f, -0.4f};
    for (int i = 0; i < 12; ++i) {
      NudgeBy(p, Nudge::Across, -0.05f);
    }
    for (int i = 0; i < 12; ++i) {
      NudgeBy(p, Nudge::Across, 0.05f);
    }
    Check(p.mPosition[0] == 0.0f && !std::signbit(p.mPosition[0]), std::format("12 steps left and back: exactly 0 ({})", p.mPosition[0]));
    NudgeBy(p, Nudge::Away, -1.0f);
    Check(std::abs(-p.mPosition[2] - 0.1f) < 0.0001f, "never nearer than 10 cm");
    p.mFaceOrigin = true;
    p.mPosition = {0.3f, 0, -0.3f};
    NudgeBy(p, Nudge::Turn, 1.0f);
    // On the right, it faces you turned 45 degrees to its right (-45)
    Check(!p.mFaceOrigin && std::abs(p.mYawDegrees + 44.0f) < 0.01f,
      std::format("turning one that faces you starts from its facing (-45 + 1 = {:.2f})", p.mYawDegrees));
    p.mYawDegrees = 179;
    NudgeBy(p, Nudge::Turn, 3.0f);
    Check(std::abs(p.mYawDegrees + 178.0f) < 0.01f, std::format("turning past 180 wraps round ({:.2f})", p.mYawDegrees));
    NudgeBy(p, Nudge::Tilt, 200.0f);
    Check(p.mPitchDegrees == 90.0f, "tilt stops at 90");
  });

  return Testing::Finish();
}
