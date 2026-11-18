// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Tests for the virtual keyboard's layout engine, which reads the installed
// Windows keyboard layout DLLs. Run it from a terminal; it exits with 0 if
// every check passes. Layouts that aren't installed on this machine are
// skipped rather than failed.

#include <OpenDesktopVR/KeyboardLayout.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace OpenDesktopVR::KeyboardLayout;

namespace {

int gFailures = 0;
int gChecks = 0;
int gSkipped = 0;

void Check(bool condition, const std::string& what) {
  ++gChecks;
  if (condition) {
    std::printf("    ok:   %s\n", what.c_str());
    return;
  }
  ++gFailures;
  std::printf("    FAIL: %s\n", what.c_str());
}

void Scenario(const char* name, const std::function<void()>& body) {
  std::printf("%s\n", name);
  body();
}

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const auto size = WideCharToMultiByte(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  std::string ret(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    ret.data(),
    size,
    nullptr,
    nullptr);
  return ret;
}

std::string Describe(const KeyOutput& output) {
  switch (output.mKind) {
    case KeyOutput::Kind::None:
      return "nothing";
    case KeyOutput::Kind::Text:
      return std::format("'{}'", ToUtf8(output.mText));
    case KeyOutput::Kind::DeadKey:
      return std::format("dead '{}'", ToUtf8(output.mText));
  }
  return "?";
}

constexpr ModifierState Plain {};
constexpr ModifierState Shift {.mShift = true};
constexpr ModifierState AltGr {.mAltGr = true};
constexpr ModifierState Caps {.mCapsLock = true};
constexpr ModifierState CapsShift {.mShift = true, .mCapsLock = true};

void CheckText(
  const Layout& layout,
  ScanCode scanCode,
  const ModifierState& state,
  std::wstring_view expected,
  const std::string& what) {
  const auto output = layout.GetOutput(scanCode, state);
  Check(
    output.mKind == KeyOutput::Kind::Text && output.mText == expected,
    std::format(
      "{} -> '{}' (got {})", what, ToUtf8(expected), Describe(output)));
}

void CheckDead(
  const Layout& layout,
  ScanCode scanCode,
  const ModifierState& state,
  wchar_t expected,
  const std::string& what) {
  const auto output = layout.GetOutput(scanCode, state);
  Check(
    output.mKind == KeyOutput::Kind::DeadKey
      && output.mText == std::wstring(1, expected),
    std::format(
      "{} -> dead '{}' (got {})",
      what,
      ToUtf8(std::wstring(1, expected)),
      Describe(output)));
}

void CheckNone(
  const Layout& layout,
  ScanCode scanCode,
  const ModifierState& state,
  const std::string& what) {
  const auto output = layout.GetOutput(scanCode, state);
  Check(
    output.mKind == KeyOutput::Kind::None,
    std::format("{} types nothing (got {})", what, Describe(output)));
}

struct KeyPress {
  ScanCode mScanCode {};
  ModifierState mState {};
};
using Keys = std::vector<KeyPress>;

// Types the keys, returning everything typed, and what's left pending
std::pair<std::wstring, std::optional<wchar_t>> Type(
  const std::shared_ptr<const Layout>& layout,
  const Keys& keys) {
  Typist typist(layout);
  std::wstring text;
  for (const auto& [scanCode, state]: keys) {
    const auto result = typist.Press(scanCode, state);
    text += result.mText;
    if (result.mPendingDeadKey != typist.GetPendingDeadKey()) {
      return {L"<result and typist disagree on the pending key>", {}};
    }
  }
  return {text, typist.GetPendingDeadKey()};
}

void CheckTyped(
  const std::shared_ptr<const Layout>& layout,
  const Keys& keys,
  std::wstring_view expected,
  const std::string& what) {
  const auto [text, pending] = Type(layout, keys);
  Check(
    text == expected && !pending,
    std::format(
      "{} -> '{}' (got '{}'{})",
      what,
      ToUtf8(expected),
      ToUtf8(text),
      pending ? std::format(", '{}' pending", ToUtf8(std::wstring(1, *pending)))
              : std::string {}));
}

void WithLayout(
  const char* klid,
  const char* name,
  const std::function<void(const std::shared_ptr<const Layout>&)>& body) {
  std::printf("%s (%s)\n", name, klid);
  if (!GetLayoutInfo(klid)) {
    ++gSkipped;
    std::printf("    skip: not installed on this machine\n");
    return;
  }
  const auto layout = Layout::Load(klid);
  Check(layout != nullptr, "loads");
  if (layout) {
    body(layout);
  }
}

// Every key and shift state of the user's own layouts against Windows' own
// ToUnicodeEx(), which only works for loaded layouts, i.e. the user's.
void CompareWithWindows(HKL hkl, const std::string& klid) {
  const auto layout = Layout::Load(klid);
  if (!layout) {
    Check(false, std::format("{} loads", klid));
    return;
  }

  std::vector<ModifierState> states {Plain, Shift, Caps, CapsShift};
  if (layout->HasAltGr()) {
    states.push_back(AltGr);
    states.push_back({.mShift = true, .mAltGr = true});
    states.push_back({.mAltGr = true, .mCapsLock = true});
  }

  int compared = 0;
  std::vector<std::string> mismatches;
  for (ScanCode code = 1; code < 0x80; ++code) {
    for (const auto prefix: {ScanCode {0}, ScanCode {0xe000}}) {
      const ScanCode scanCode = prefix | code;
      const auto vk = layout->GetNumLockVirtualKey(scanCode);
      if (!vk) {
        continue;
      }
      // Windows' own mapping agrees with ours
      const auto windowsVK = MapVirtualKeyExW(
        prefix ? (0xe000u | code) : code, MAPVK_VSC_TO_VK_EX, hkl);
      if (windowsVK != 0 && windowsVK != layout->GetVirtualKey(scanCode)) {
        mismatches.push_back(std::format(
          "scan {:04X}: VK {:02X}, Windows says {:02X}",
          scanCode,
          layout->GetVirtualKey(scanCode),
          windowsVK));
      }

      for (const auto& state: states) {
        std::array<BYTE, 256> keyState {};
        if (state.mShift) {
          keyState[VK_SHIFT] = keyState[VK_LSHIFT] = 0x80;
        }
        if (state.mAltGr) {
          keyState[VK_CONTROL] = keyState[VK_LCONTROL] = 0x80;
          keyState[VK_MENU] = keyState[VK_RMENU] = 0x80;
        }
        if (state.mCapsLock) {
          keyState[VK_CAPITAL] = 0x01;
        }
        keyState[VK_NUMLOCK] = 0x01;
        std::array<wchar_t, 16> buffer {};
        // 0x4: don't change the keyboard (dead key) state
        const auto count = ToUnicodeEx(
          vk,
          code,
          keyState.data(),
          buffer.data(),
          static_cast<int>(buffer.size()),
          0x4,
          hkl);
        KeyOutput expected;
        if (count < 0) {
          expected = {KeyOutput::Kind::DeadKey, std::wstring(1, buffer[0])};
        } else if (count > 0) {
          std::wstring text(buffer.data(), static_cast<std::size_t>(count));
          if (std::ranges::none_of(text, [](wchar_t c) {
                return c < 0x20 || c == 0x7f;
              })) {
            expected = {KeyOutput::Kind::Text, std::move(text)};
          }
        }
        const auto actual = layout->GetOutput(scanCode, state);
        ++compared;
        if (actual != expected) {
          mismatches.push_back(std::format(
            "scan {:04X} shift={} altgr={} caps={}: {}, Windows says {}",
            scanCode,
            state.mShift,
            state.mAltGr,
            state.mCapsLock,
            Describe(actual),
            Describe(expected)));
        }
      }
    }
  }
  for (std::size_t i = 0; i < mismatches.size() && i < 20; ++i) {
    std::printf("      %s\n", mismatches[i].c_str());
  }
  Check(
    mismatches.empty(),
    std::format(
      "{}: {} key/shift combinations match ToUnicodeEx ({} mismatches)",
      klid,
      compared,
      mismatches.size()));
}

}// namespace

int main() {
  SetConsoleOutputCP(CP_UTF8);

  Scenario("Layout list", [] {
    const auto layouts = GetAvailableLayouts();
    Check(
      layouts.size() > 50,
      std::format("lots of layouts are available ({})", layouts.size()));
    const auto find = [&](std::string_view klid) -> const LayoutInfo* {
      const auto it = std::ranges::find(layouts, klid, &LayoutInfo::mKLID);
      return it == layouts.end() ? nullptr : &*it;
    };
    const auto us = find("00000409");
    Check(
      us && us->mDisplayName == L"US" && us->mLayoutFile == L"KBDUS.DLL",
      std::format(
        "00000409 is 'US' in KBDUS.DLL (got '{}')",
        us ? ToUtf8(us->mDisplayName) : "missing"));
    const auto colemak = find("00060409");
    Check(
      colemak && colemak->mDisplayName.contains(L"Colemak"),
      std::format(
        "00060409 is Colemak (got '{}')",
        colemak ? ToUtf8(colemak->mDisplayName) : "missing"));
    Check(
      std::ranges::is_sorted(
        layouts,
        [](const LayoutInfo& a, const LayoutInfo& b) {
          return CompareStringEx(
                   LOCALE_NAME_USER_DEFAULT,
                   LINGUISTIC_IGNORECASE,
                   a.mDisplayName.c_str(),
                   -1,
                   b.mDisplayName.c_str(),
                   -1,
                   nullptr,
                   nullptr,
                   0)
            == CSTR_LESS_THAN;
        }),
      "sorted by name");
    Check(
      std::ranges::none_of(
        layouts,
        [](const LayoutInfo& info) {
          return info.mDisplayName.empty() || info.mDisplayName.starts_with(L'@')
            || info.mKLID.size() != 8;
        }),
      "every layout has a resolved name and an 8-digit KLID");
    const auto info = GetLayoutInfo("00000809");
    Check(
      info && info->mLayoutFile == L"KBDUK.DLL",
      "GetLayoutInfo(00000809) is KBDUK.DLL");
    Check(!GetLayoutInfo("12345678"), "GetLayoutInfo(12345678) is nullopt");
    Check(!GetLayoutInfo("garbage"), "GetLayoutInfo(garbage) is nullopt");
    Check(!Layout::Load("12345678"), "Load(12345678) is nullptr");
    Check(!Layout::Load(""), "Load('') is nullptr");
  });

  Scenario("HKL -> KLID", [] {
    const auto current = GetKeyboardLayout(0);
    const auto klid = GetKLID(current);
    std::array<wchar_t, KL_NAMELENGTH> name {};
    GetKeyboardLayoutNameW(name.data());
    Check(
      klid == ToUtf8(name.data()),
      std::format(
        "current layout {:08X} -> {} (GetKeyboardLayoutName: {})",
        reinterpret_cast<uintptr_t>(current) & 0xffffffff,
        klid,
        ToUtf8(name.data())));
    const auto user = GetUserLayoutKLIDs();
    std::string list;
    for (const auto& it: user) {
      list += it + " ";
    }
    Check(!user.empty(), std::format("user has layouts: {}", list));
    Check(
      !user.empty() && user.front() == klid,
      "the first user layout is the current one");
    Check(
      GetKLID(reinterpret_cast<HKL>(0x04090409)) == "00000409",
      "04090409 -> 00000409");
    Check(
      GetKLID(reinterpret_cast<HKL>(0x04070409)) == "00000407",
      "04070409 (German layout, English language) -> 00000407");
    Check(
      GetKLID(reinterpret_cast<HKL>(static_cast<uintptr_t>(0xf0020409)))
        == "00010409",
      "F0020409 (layout ID 0002) -> 00010409 (Dvorak)");
    Check(
      GetKLID(reinterpret_cast<HKL>(static_cast<uintptr_t>(0xf0d30409)))
        == "00060409",
      "F0D30409 (layout ID 00d3) -> 00060409 (Colemak)");
    Check(GetKLID(nullptr).empty(), "null HKL -> ''");
  });

  WithLayout("00000409", "US", [](const auto& us) {
    CheckText(*us, 0x10, Plain, L"q", "Q");
    CheckText(*us, 0x03, Shift, L"@", "Shift+2");
    CheckText(*us, 0x39, Plain, L" ", "Space");
    CheckText(*us, 0x56, Plain, L"\\", "the (non-existent) ISO key");
    Check(!us->IsISO(), "is not ISO");
    Check(!us->HasAltGr(), "has no AltGr");
    Check(us->GetVirtualKey(0x10) == 'Q', "scan 10 is VK 'Q'");
    Check(us->GetVirtualKey(0xe01d) == VK_RCONTROL, "scan E01D is VK_RCONTROL");
    Check(us->GetVirtualKey(0xe048) == VK_UP, "scan E048 is VK_UP");
    Check(us->GetVirtualKey(0x48) == VK_UP, "scan 48 (numpad 8) is VK_UP");
    Check(us->GetVirtualKey(0xe11d) == VK_PAUSE, "scan E11D is VK_PAUSE");
    Check(us->GetVirtualKey(0x7f) == 0, "scan 7F is unmapped");
    Check(us->GetNumLockVirtualKey(0x48) == VK_NUMPAD8, "with NumLock, scan 48 is VK_NUMPAD8");
    Check(us->GetNumLockVirtualKey(0xe048) == VK_UP, "with NumLock, scan E048 is still VK_UP");
    Check(us->IsNumpadKey(0x48) && !us->IsNumpadKey(0xe048), "scan 48 is a numpad key, E048 isn't");
    Check(us->GetScanCode('Q') == 0x10, "VK 'Q' is scan 10");
    Check(us->GetScanCode(VK_HOME) == 0xe047, "VK_HOME is scan E047, not the numpad");
    Check(us->GetScanCode(VK_NUMPAD7) == 0x47, "VK_NUMPAD7 is scan 47");
    Check(us->GetScanCode(VK_DECIMAL) == 0x53, "VK_DECIMAL is scan 53");
    Check(us->GetScanCode(VK_RCONTROL) == 0xe01d, "VK_RCONTROL is scan E01D");
    Check(us->GetScanCode(VK_RETURN) == 0x1c, "VK_RETURN is scan 1C");
    Check(us->GetScanCode(VK_DIVIDE) == 0xe035, "VK_DIVIDE is scan E035");
    Check(!us->GetScanCode(0xff), "VK FF has no scan code");
    Check(us->GetScanCode(VK_SHIFT) == 0x2a, "VK_SHIFT is scan 2A");
    Check(us->GetScanCode(VK_RSHIFT) == 0x36, "VK_RSHIFT is scan 36");
    Check(us->GetScanCode(VK_RMENU) == 0xe038, "VK_RMENU is scan E038");
    CheckText(*us, 0x47, Plain, L"7", "numpad 7");
    CheckText(*us, 0x53, Plain, L".", "numpad .");
    CheckText(*us, 0xe035, Plain, L"/", "numpad /");
    CheckText(*us, 0x37, Plain, L"*", "numpad *");
    CheckNone(*us, 0x1c, Plain, "Enter");
    CheckNone(*us, 0xe01c, Plain, "numpad Enter");
    CheckNone(*us, 0x0f, Plain, "Tab");
    CheckNone(*us, 0x0e, Plain, "Backspace");
    CheckNone(*us, 0x01, Plain, "Escape");
    CheckNone(*us, 0x10, AltGr, "AltGr+Q");
    CheckNone(*us, 0xe048, Plain, "Up");
  });

  Scenario("CapsLock (US)", [] {
    const auto us = Layout::Load("00000409");
    if (!us) {
      Check(false, "US loads");
      return;
    }
    CheckText(*us, 0x1e, Caps, L"A", "Caps+A");
    CheckText(*us, 0x1e, CapsShift, L"a", "Caps+Shift+A");
    CheckText(*us, 0x02, Caps, L"1", "Caps+1");
    CheckText(*us, 0x02, CapsShift, L"!", "Caps+Shift+1");
  });

  WithLayout("00000809", "UK", [](const auto& uk) {
    CheckText(*uk, 0x03, Shift, L"\"", "Shift+2");
    CheckText(*uk, 0x04, Shift, L"£", "Shift+3");
    CheckText(*uk, 0x2b, Plain, L"#", "the key by Enter");
    CheckText(*uk, 0x56, Plain, L"\\", "the ISO key");
    CheckText(*uk, 0x05, AltGr, L"€", "AltGr+4");
    Check(uk->IsISO(), "is ISO");
    Check(uk->HasAltGr(), "has AltGr");
  });

  WithLayout("00000407", "German", [](const auto& de) {
    CheckText(*de, 0x15, Plain, L"z", "scan 15 (US Y)");
    CheckText(*de, 0x2c, Plain, L"y", "scan 2C (US Z)");
    CheckText(*de, 0x10, AltGr, L"@", "AltGr+Q");
    CheckText(*de, 0x0c, Plain, L"ß", "scan 0C");
    CheckText(*de, 0x1a, Plain, L"ü", "scan 1A");
    CheckText(*de, 0x1a, Caps, L"Ü", "Caps+scan 1A");
    CheckText(*de, 0x56, Plain, L"<", "the ISO key");
    CheckText(*de, 0x56, AltGr, L"|", "AltGr+the ISO key");
    CheckDead(*de, 0x29, Plain, L'^', "scan 29");
    Check(de->IsISO(), "is ISO");
    Check(de->HasAltGr(), "has AltGr");
    Check(de->Compose(L'^', L'e') == L'ê', "Compose('^', 'e') is ê");
    Check(!de->Compose(L'^', L'q'), "Compose('^', 'q') is nullopt");
    CheckTyped(de, {{0x29, Plain}, {0x12, Plain}}, L"ê", "^ e");
    CheckTyped(de, {{0x29, Plain}, {0x12, Shift}}, L"Ê", "^ E");
    CheckTyped(de, {{0x0d, Plain}, {0x12, Plain}}, L"é", "´ e");
    CheckTyped(de, {{0x0d, Shift}, {0x1e, Plain}}, L"à", "` a");
    CheckTyped(de, {{0x29, Plain}, {0x39, Plain}}, L"^", "^ space");
    CheckTyped(de, {{0x29, Plain}, {0x10, Plain}}, L"^q", "^ q");
    CheckTyped(de, {{0x29, Plain}, {0x0d, Plain}}, L"^´", "^ ´");
    {
      const auto [text, pending] = Type(de, {{0x29, Plain}, {0x29, Plain}});
      Check(
        !text.empty() && !pending,
        std::format("^ ^ -> '{}' (and nothing pending)", ToUtf8(text)));
    }
    {
      Typist typist(de);
      const auto first = typist.Press(0x29, Plain);
      Check(
        first.mText.empty() && first.mPendingDeadKey == L'^',
        "^ is pending after pressing it");
      const auto shift = typist.Press(0x2a, Shift);
      Check(
        shift.mText.empty() && shift.mPendingDeadKey == L'^',
        "pressing Shift on its own leaves it pending");
      const auto back = typist.Press(0x0e, Plain);
      Check(
        back.mText.empty() && !back.mPendingDeadKey,
        "Backspace drops it");
      typist.Press(0x29, Plain);
      typist.CancelDeadKey();
      Check(!typist.GetPendingDeadKey(), "CancelDeadKey() drops it");
      const auto after = typist.Press(0x12, Plain);
      Check(after.mText == L"e", "then e types e");
    }
  });

  WithLayout("0000040C", "French", [](const auto& fr) {
    CheckText(*fr, 0x10, Plain, L"a", "scan 10 (US Q)");
    CheckText(*fr, 0x1e, Plain, L"q", "scan 1E (US A)");
    CheckText(*fr, 0x2c, Plain, L"w", "scan 2C (US Z)");
    CheckText(*fr, 0x32, Plain, L",", "scan 32 (US M)");
    const std::wstring_view numberRow = L"&é\"'(-è_çà";
    for (ScanCode i = 0; i < numberRow.size(); ++i) {
      CheckText(
        *fr,
        static_cast<ScanCode>(0x02 + i),
        Plain,
        numberRow.substr(i, 1),
        std::format("number row key {}", (i + 1) % 10));
    }
    CheckText(*fr, 0x03, Shift, L"2", "Shift+2");
    CheckDead(*fr, 0x1a, Plain, L'^', "scan 1A");
    CheckTyped(fr, {{0x1a, Plain}, {0x12, Plain}}, L"ê", "^ e");
    CheckTyped(fr, {{0x1a, Shift}, {0x12, Plain}}, L"ë", "¨ e");
    Check(fr->IsISO(), "is ISO");
  });

  WithLayout("00010409", "US-Dvorak", [](const auto& dv) {
    CheckText(*dv, 0x10, Plain, L"'", "scan 10 (US Q)");
    CheckText(*dv, 0x11, Plain, L",", "scan 11 (US W)");
    CheckText(*dv, 0x1f, Plain, L"o", "scan 1F (US S)");
    Check(dv->GetVirtualKey(0x1f) == 'O', "scan 1F is VK 'O'");
    Check(dv->GetScanCode('O') == 0x1f, "VK 'O' is scan 1F");
    Check(!dv->IsISO(), "is not ISO");
  });

  WithLayout("00060409", "Colemak", [](const auto& cmk) {
    CheckText(*cmk, 0x12, Plain, L"f", "scan 12 (US E)");
    CheckText(*cmk, 0x13, Plain, L"p", "scan 13 (US R)");
    CheckText(*cmk, 0x1f, Plain, L"r", "scan 1F (US S)");
    CheckText(*cmk, 0x27, Plain, L"o", "scan 27 (US ;)");
  });

  WithLayout("00000419", "Russian", [](const auto& ru) {
    CheckText(*ru, 0x10, Plain, L"й", "scan 10 (US Q)");
    CheckText(*ru, 0x10, Shift, L"Й", "Shift+scan 10");
    CheckText(*ru, 0x10, Caps, L"Й", "Caps+scan 10");
    CheckText(*ru, 0x04, Shift, L"№", "Shift+3");
  });

  WithLayout("00020409", "US-International", [](const auto& intl) {
    Check(intl->HasAltGr(), "has AltGr");
    CheckDead(*intl, 0x28, Plain, L'\'', "scan 28");
    CheckTyped(intl, {{0x28, Plain}, {0x12, Plain}}, L"é", "' e");
    CheckTyped(intl, {{0x28, Plain}, {0x39, Plain}}, L"'", "' space");
    CheckTyped(intl, {{0x28, Plain}, {0x14, Plain}}, L"'t", "' t");
    CheckTyped(intl, {{0x28, Shift}, {0x1e, Plain}}, L"ä", "\" a");
    CheckTyped(intl, {{0x29, Shift}, {0x31, Plain}}, L"ñ", "~ n");
    CheckTyped(intl, {{0x2e, Plain}, {0x28, Plain}, {0x12, Plain}}, L"cé", "c ' e");
    CheckText(*intl, 0x10, AltGr, L"ä", "AltGr+Q");
    Check(
      intl->GetSpacingDeadChar(L'\'') == L'\'',
      "the spacing version of ' is '");
  });

  WithLayout("00000807", "Swiss German (SGCAPS)", [](const auto& sg) {
    CheckText(*sg, 0x1a, Plain, L"ü", "scan 1A");
    CheckText(*sg, 0x1a, Shift, L"è", "Shift+scan 1A");
    CheckText(*sg, 0x1a, Caps, L"Ü", "Caps+scan 1A");
    CheckText(*sg, 0x1a, CapsShift, L"È", "Caps+Shift+scan 1A");
  });

  WithLayout("00000405", "Czech (SGCAPS sharing a row with dead keys)", [](const auto& cz) {
    CheckText(*cz, 0x04, Plain, L"š", "scan 04");
    CheckText(*cz, 0x04, Shift, L"3", "Shift+scan 04");
    CheckText(*cz, 0x04, Caps, L"Š", "Caps+scan 04");
    CheckText(*cz, 0x04, CapsShift, L"3", "Caps+Shift+scan 04");
    CheckDead(*cz, 0x04, AltGr, L'^', "AltGr+scan 04");
  });

  Scenario("Every installed layout", [] {
    const auto listStart = std::chrono::steady_clock::now();
    const auto layouts = GetAvailableLayouts();
    const auto listTime = std::chrono::steady_clock::now() - listStart;
    std::chrono::steady_clock::duration loadTime {};
    int loaded = 0;
    int iso = 0;
    int altGr = 0;
    int withDeadKeys = 0;
    int withLigatures = 0;
    int deadWithoutSpace = 0;
    std::vector<std::string> failed;
    std::vector<std::string> bad;
    for (const auto& info: layouts) {
      const auto loadStart = std::chrono::steady_clock::now();
      const auto layout = Layout::Load(info.mKLID);
      loadTime += std::chrono::steady_clock::now() - loadStart;
      if (!layout) {
        failed.push_back(
          std::format("{} {}", info.mKLID, ToUtf8(info.mLayoutFile)));
        continue;
      }
      ++loaded;
      iso += layout->IsISO();
      altGr += layout->HasAltGr();
      bool dead = false;
      bool ligature = false;
      int letters = 0;
      for (ScanCode code = 1; code < 0x80; ++code) {
        for (const auto prefix: {ScanCode {0}, ScanCode {0xe000}}) {
          const ScanCode scanCode = prefix | code;
          for (int i = 0; i < 8; ++i) {
            const ModifierState state {
              .mShift = (i & 1) != 0,
              .mAltGr = (i & 2) != 0,
              .mCapsLock = (i & 4) != 0,
            };
            const auto output = layout->GetOutput(scanCode, state);
            if (output.mKind == KeyOutput::Kind::None) {
              continue;
            }
            if (
              output.mText.empty()
              || std::ranges::any_of(
                output.mText,
                [](wchar_t c) {
                  return c < 0x20 || c == 0x7f || (c >= 0xf000 && c <= 0xf002);
                })
              || (output.mKind == KeyOutput::Kind::DeadKey
                  && output.mText.size() != 1)) {
              bad.push_back(std::format(
                "{}: scan {:04X} state {} -> {}",
                info.mKLID,
                scanCode,
                i,
                Describe(output)));
            }
            if (output.mKind == KeyOutput::Kind::DeadKey) {
              dead = true;
              if (!layout->Compose(output.mText.front(), L' ')) {
                ++deadWithoutSpace;
              }
            }
            ligature |= output.mText.size() > 1;
            letters += (code >= 0x10 && code <= 0x32 && !prefix);
          }
        }
        // Round trip
        if (const auto vk = layout->GetVirtualKey(code);
            vk && layout->GetScanCode(vk) != code
            && layout->GetVirtualKey(*layout->GetScanCode(vk)) != vk) {
          failed.push_back(
            std::format("{}: scan {:02X} round trip", info.mKLID, code));
        }
      }
      withDeadKeys += dead;
      withLigatures += ligature;
      if (letters < 26) {
        bad.push_back(std::format(
          "{} {} ({}): only {} characters on the letter keys",
          info.mKLID,
          ToUtf8(info.mDisplayName),
          ToUtf8(info.mLayoutFile),
          letters));
      }
    }
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    std::printf(
      "    %d layouts: %d ISO, %d with AltGr, %d with dead keys, %d with "
      "ligatures; %d dead key/shift states have no dead+space entry\n"
      "    listing took %lldms, loading all took %lldms\n",
      loaded,
      iso,
      altGr,
      withDeadKeys,
      withLigatures,
      deadWithoutSpace,
      static_cast<long long>(duration_cast<milliseconds>(listTime).count()),
      static_cast<long long>(duration_cast<milliseconds>(loadTime).count()));
    for (std::size_t i = 0; i < failed.size() && i < 20; ++i) {
      std::printf("      %s\n", failed[i].c_str());
    }
    Check(failed.empty(), std::format("all {} layouts load", layouts.size()));
    for (std::size_t i = 0; i < bad.size() && i < 20; ++i) {
      std::printf("      %s\n", bad[i].c_str());
    }
    Check(
      bad.empty(),
      "no layout types control characters or WCH_* values, and all have "
      "letters");
  });

  Scenario("The user's layouts match ToUnicodeEx", [] {
    const auto count = GetKeyboardLayoutList(0, nullptr);
    std::vector<HKL> hkls(static_cast<std::size_t>((std::max)(count, 0)));
    GetKeyboardLayoutList(count, hkls.data());
    for (const auto hkl: hkls) {
      const auto klid = GetKLID(hkl);
      if (klid.empty()) {
        std::printf(
          "    skip: no KLID for HKL %08llX\n",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hkl)));
        continue;
      }
      CompareWithWindows(hkl, klid);
    }
  });

  std::printf(
    "\n%d checks, %d failed, %d layouts skipped\n",
    gChecks,
    gFailures,
    gSkipped);
  return gFailures == 0 ? 0 : 1;
}
