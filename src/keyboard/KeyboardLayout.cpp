// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Reads the keyboard layout tables (KBDTABLES, from the Windows SDK's kbd.h)
// out of the installed layout DLLs, copies them, and then interprets them the
// way Windows does when it turns a key press into characters.

#include <OpenDesktopVR/KeyboardLayout.hpp>

#include <Shlwapi.h>

#include <kbd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <format>
#include <unordered_map>
#include <utility>

namespace OpenDesktopVR::KeyboardLayout {

namespace {

constexpr auto LayoutsKey
  = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";

// Sanity limits while copying the tables; real layouts are far below these
constexpr std::size_t MaxTables = 64;
constexpr std::size_t MaxRowsPerTable = 1024;
constexpr std::size_t MaxModifications = 32;
constexpr std::size_t MaxDeadKeys = 16384;
constexpr std::size_t MaxLigatures = 4096;
constexpr std::size_t MaxExtendedScanCodes = 256;

constexpr uint8_t AttachedRowVK = 0xff;// dead key / SGCAPS rows

// With NumLock on, numpad keys turn into these
struct NumpadVK {
  uint8_t mNumLockOff {};
  uint8_t mNumLockOn {};
};
constexpr std::array<NumpadVK, 11> NumpadVKs {{
  {VK_INSERT, VK_NUMPAD0},
  {VK_END, VK_NUMPAD1},
  {VK_DOWN, VK_NUMPAD2},
  {VK_NEXT, VK_NUMPAD3},
  {VK_LEFT, VK_NUMPAD4},
  {VK_CLEAR, VK_NUMPAD5},
  {VK_RIGHT, VK_NUMPAD6},
  {VK_HOME, VK_NUMPAD7},
  {VK_UP, VK_NUMPAD8},
  {VK_PRIOR, VK_NUMPAD9},
  {VK_DELETE, VK_DECIMAL},
}};

// kbd.h's DEADTRANS() uses MAKELONG(base, accent)
constexpr uint32_t DeadKeyID(wchar_t accent, wchar_t base) {
  return (static_cast<uint32_t>(accent) << 16) | static_cast<uint32_t>(base);
}

bool IsControlCharacter(wchar_t c) {
  return c < 0x20 || c == 0x7f;
}

std::optional<std::wstring> ReadString(HKEY key, const wchar_t* name) {
  DWORD bytes {};
  if (
    RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes)
    != ERROR_SUCCESS) {
    return std::nullopt;
  }
  std::wstring ret(bytes / sizeof(wchar_t) + 1, L'\0');
  bytes = static_cast<DWORD>(ret.size() * sizeof(wchar_t));
  if (
    RegGetValueW(
      key, nullptr, name, RRF_RT_REG_SZ, nullptr, ret.data(), &bytes)
    != ERROR_SUCCESS) {
    return std::nullopt;
  }
  ret.resize(wcsnlen(ret.data(), ret.size()));
  if (ret.find(L'%') != std::wstring::npos) {
    const auto chars = ExpandEnvironmentStringsW(ret.c_str(), nullptr, 0);
    if (chars > 0) {
      std::wstring expanded(chars, L'\0');
      if (
        ExpandEnvironmentStringsW(ret.c_str(), expanded.data(), chars)
        == chars) {
        expanded.resize(chars - 1);
        ret = std::move(expanded);
      }
    }
  }
  return ret;
}

struct RegKey {
  HKEY mKey {nullptr};
  RegKey() = default;
  RegKey(const RegKey&) = delete;
  RegKey& operator=(const RegKey&) = delete;
  ~RegKey() {
    if (mKey) {
      RegCloseKey(mKey);
    }
  }
  bool Open(HKEY parent, const wchar_t* path) {
    return RegOpenKeyExW(parent, path, 0, KEY_READ, &mKey) == ERROR_SUCCESS;
  }
};

std::wstring Widen(std::string_view s) {
  return {s.begin(), s.end()};
}

bool IsValidKLID(std::string_view klid) {
  return klid.size() == 8 && std::ranges::all_of(klid, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
             || (c >= 'A' && c <= 'F');
         });
}

std::wstring GetSystem32Path() {
  std::array<wchar_t, MAX_PATH> buf {};
  const auto len
    = GetSystemDirectoryW(buf.data(), static_cast<UINT>(buf.size()));
  if (len == 0 || len >= buf.size()) {
    return {};
  }
  return {buf.data(), len};
}

// Only a bare file name: we always load from System32
bool IsPlainFileName(std::wstring_view file) {
  return !file.empty()
    && file.find_first_of(L"\\/:") == std::wstring_view::npos
    && file != L"." && file != L"..";
}

bool LayoutFileExists(const std::wstring& systemDir, const std::wstring& file) {
  if (systemDir.empty() || !IsPlainFileName(file)) {
    return false;
  }
  const auto attributes = GetFileAttributesW((systemDir + L"\\" + file).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES
    && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ResolveDisplayName(const std::wstring& indirect) {
  if (indirect.empty() || indirect.front() != L'@') {
    return indirect;
  }
  std::array<wchar_t, 512> buf {};
  if (
    SHLoadIndirectString(
      indirect.c_str(), buf.data(), static_cast<UINT>(buf.size()), nullptr)
    != S_OK) {
    return {};
  }
  return buf.data();
}

std::optional<LayoutInfo> ReadLayoutInfo(
  HKEY layoutsKey,
  const std::wstring& klid,
  const std::wstring& systemDir) {
  RegKey key;
  if (!key.Open(layoutsKey, klid.c_str())) {
    return std::nullopt;
  }
  auto file = ReadString(key.mKey, L"Layout File");
  if (!file || !LayoutFileExists(systemDir, *file)) {
    return std::nullopt;
  }

  std::wstring name;
  if (const auto display = ReadString(key.mKey, L"Layout Display Name")) {
    name = ResolveDisplayName(*display);
  }
  if (name.empty()) {
    name = ReadString(key.mKey, L"Layout Text").value_or(std::wstring {});
  }
  if (name.empty()) {
    name = klid;
  }

  LayoutInfo ret {
    .mDisplayName = std::move(name),
    .mLayoutFile = std::move(*file),
  };
  ret.mKLID.reserve(klid.size());
  for (const auto c: klid) {
    ret.mKLID.push_back(static_cast<char>(std::towupper(c)));
  }
  return ret;
}

std::vector<std::wstring> GetRegistryKLIDs(HKEY layoutsKey) {
  std::vector<std::wstring> ret;
  for (DWORD i = 0;; ++i) {
    std::array<wchar_t, 256> name {};
    auto nameLen = static_cast<DWORD>(name.size());
    const auto result = RegEnumKeyExW(
      layoutsKey, i, name.data(), &nameLen, nullptr, nullptr, nullptr, nullptr);
    if (result == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (result != ERROR_SUCCESS) {
      continue;
    }
    std::wstring klid {name.data(), nameLen};
    if (
      klid.size() == 8 && std::ranges::all_of(klid, [](wchar_t c) {
        return std::iswxdigit(c) && c < 0x80;
      })) {
      ret.push_back(std::move(klid));
    }
  }
  return ret;
}

std::string Narrow(std::wstring_view s) {
  std::string ret;
  ret.reserve(s.size());
  for (const auto c: s) {
    ret.push_back(static_cast<char>(std::towupper(c)));
  }
  return ret;
}

bool KLIDExists(HKEY layoutsKey, const std::string& klid) {
  RegKey key;
  return key.Open(layoutsKey, Widen(klid).c_str());
}

}// namespace

std::vector<LayoutInfo> GetAvailableLayouts() {
  RegKey layouts;
  if (!layouts.Open(HKEY_LOCAL_MACHINE, LayoutsKey)) {
    return {};
  }
  const auto systemDir = GetSystem32Path();

  std::vector<LayoutInfo> ret;
  for (const auto& klid: GetRegistryKLIDs(layouts.mKey)) {
    if (auto info = ReadLayoutInfo(layouts.mKey, klid, systemDir)) {
      ret.push_back(std::move(*info));
    }
  }

  std::ranges::sort(ret, [](const LayoutInfo& a, const LayoutInfo& b) {
    const auto order = CompareStringEx(
      LOCALE_NAME_USER_DEFAULT,
      LINGUISTIC_IGNORECASE,
      a.mDisplayName.c_str(),
      static_cast<int>(a.mDisplayName.size()),
      b.mDisplayName.c_str(),
      static_cast<int>(b.mDisplayName.size()),
      nullptr,
      nullptr,
      0);
    if (order == CSTR_EQUAL || order == 0) {
      return a.mKLID < b.mKLID;
    }
    return order == CSTR_LESS_THAN;
  });
  return ret;
}

std::optional<LayoutInfo> GetLayoutInfo(std::string_view klid) {
  if (!IsValidKLID(klid)) {
    return std::nullopt;
  }
  RegKey layouts;
  if (!layouts.Open(HKEY_LOCAL_MACHINE, LayoutsKey)) {
    return std::nullopt;
  }
  return ReadLayoutInfo(layouts.mKey, Widen(klid), GetSystem32Path());
}

std::string GetKLID(HKL hkl) {
  const auto value
    = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hkl) & 0xffffffff);
  if (value == 0) {
    return {};
  }
  const auto high = static_cast<uint16_t>(value >> 16);

  RegKey layouts;
  if (!layouts.Open(HKEY_LOCAL_MACHINE, LayoutsKey)) {
    return {};
  }

  if ((high & 0xf000) == 0xf000) {
    // Layouts that share a language (e.g. US-Dvorak, 00010409) have a
    // "Layout Id" in the registry, and the HKL's high word is 0xF000 | that
    const auto layoutId = static_cast<uint16_t>(high & 0x0fff);
    for (const auto& klid: GetRegistryKLIDs(layouts.mKey)) {
      RegKey key;
      if (!key.Open(layouts.mKey, klid.c_str())) {
        continue;
      }
      const auto id = ReadString(key.mKey, L"Layout Id");
      if (!id || id->empty()) {
        continue;
      }
      wchar_t* end = nullptr;
      const auto parsed = std::wcstoul(id->c_str(), &end, 16);
      if (end && *end == L'\0' && parsed == layoutId) {
        return Narrow(klid);
      }
    }
    return {};
  }

  // IMEs (0xE...) are registered under the whole HKL; everything else is
  // the high word, which is the language ID for the language's default
  // layout (0x04090409 -> 00000409), or another language's layout used as a
  // substitute (0x04070409: German layout for English -> 00000407).
  const auto wholeHKL = std::format("{:08X}", value);
  const auto byHighWord = std::format("{:08X}", static_cast<uint32_t>(high));
  if ((high & 0xf000) == 0xe000) {
    return KLIDExists(layouts.mKey, wholeHKL) ? wholeHKL : std::string {};
  }
  if (KLIDExists(layouts.mKey, byHighWord)) {
    return byHighWord;
  }
  if (KLIDExists(layouts.mKey, wholeHKL)) {
    return wholeHKL;
  }
  return {};
}

std::vector<std::string> GetUserLayoutKLIDs() {
  const auto count = GetKeyboardLayoutList(0, nullptr);
  if (count <= 0) {
    return {};
  }
  std::vector<HKL> hkls(static_cast<std::size_t>(count));
  const auto actual = GetKeyboardLayoutList(count, hkls.data());
  hkls.resize(static_cast<std::size_t>((std::max)(actual, 0)));

  std::vector<std::string> ret;
  for (const auto hkl: hkls) {
    auto klid = GetKLID(hkl);
    if (klid.empty() || std::ranges::contains(ret, klid)) {
      continue;
    }
    ret.push_back(std::move(klid));
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// Layout
////////////////////////////////////////////////////////////////////////////////

struct Layout::Impl {
  LayoutInfo mInfo;
  DWORD mLocaleFlags {};

  uint8_t mShiftBit {KBDSHIFT};
  uint8_t mCtrlBit {KBDCTRL};
  uint8_t mAltBit {KBDALT};
  // Indexed by modifier bits; SHFT_INVALID for unused combinations
  std::vector<uint8_t> mModNumbers;

  struct Row {
    uint8_t mVK {};
    uint8_t mAttributes {};
    std::vector<wchar_t> mChars;// One per modification number
  };
  struct Table {
    uint8_t mModifications {};
    std::vector<Row> mRows;
  };
  std::vector<Table> mTables;

  struct RowRef {
    uint16_t mTable {};
    uint16_t mRow {};
  };
  // Windows searches the tables in order, and moves on to the next table if
  // a key has no character for the shift state; a key is almost always in
  // only one table though.
  std::array<std::vector<RowRef>, 256> mRowsForVK;

  // (VK << 16 | modification number) -> characters
  std::unordered_map<uint32_t, std::wstring> mLigatures;

  struct DeadKeyResult {
    wchar_t mComposed {};
    bool mChained {false};
  };
  // DeadKeyID(accent, base)
  std::unordered_map<uint32_t, DeadKeyResult> mDeadKeys;

  std::vector<uint16_t> mScanCodeToVK;
  std::vector<std::pair<uint8_t, uint16_t>> mE0ScanCodeToVK;
  std::vector<std::pair<uint8_t, uint16_t>> mE1ScanCodeToVK;

  bool mIsISO {false};

  uint16_t GetFlaggedVirtualKey(ScanCode) const noexcept;
  std::optional<uint8_t> GetModificationNumber(
    uint32_t modBits,
    const Table&) const noexcept;
  // The raw character (may be WCH_DEAD/WCH_LGTR) and, for dead keys, the
  // dead character
  struct Lookup {
    wchar_t mChar {WCH_NONE};
    wchar_t mDeadChar {};
    uint8_t mVK {};
    uint8_t mModification {};
  };
  Lookup LookUp(uint8_t vk, const ModifierState&) const;
  KeyOutput GetOutput(ScanCode, const ModifierState&) const;
};

namespace {

// Copies everything we need out of the DLL; reading the DLL's memory can
// fault if a table is malformed, so this is called with SEH around it.
bool CopyTables(const KBDTABLES& tables, Layout::Impl& impl) {
  impl.mLocaleFlags = tables.fLocaleFlags;

  // Modifiers
  const auto modifiers = tables.pCharModifiers;
  if (!modifiers) {
    return false;
  }
  if (const auto vkToBit = modifiers->pVkToBit) {
    for (std::size_t i = 0; i < 32 && vkToBit[i].Vk; ++i) {
      switch (vkToBit[i].Vk) {
        case VK_SHIFT:
          impl.mShiftBit = vkToBit[i].ModBits;
          break;
        case VK_CONTROL:
          impl.mCtrlBit = vkToBit[i].ModBits;
          break;
        case VK_MENU:
          impl.mAltBit = vkToBit[i].ModBits;
          break;
        default:
          break;
      }
    }
  }
  if (modifiers->wMaxModBits > 0xff) {
    return false;
  }
  impl.mModNumbers.assign(
    modifiers->ModNumber, modifiers->ModNumber + modifiers->wMaxModBits + 1);

  // Characters
  const auto wcharTables = tables.pVkToWcharTable;
  if (!wcharTables) {
    return false;
  }
  for (std::size_t t = 0; t < MaxTables && wcharTables[t].pVkToWchars; ++t) {
    const auto& source = wcharTables[t];
    const auto mods = static_cast<std::size_t>(source.nModifications);
    if (
      mods == 0 || mods > MaxModifications
      || source.cbSize < offsetof(VK_TO_WCHARS1, wch) + (mods * sizeof(WCHAR))) {
      continue;
    }
    Layout::Impl::Table table {.mModifications = static_cast<uint8_t>(mods)};
    auto bytes = reinterpret_cast<const std::byte*>(source.pVkToWchars);
    for (std::size_t r = 0; r < MaxRowsPerTable; ++r, bytes += source.cbSize) {
      const auto row = reinterpret_cast<const VK_TO_WCHARS1*>(bytes);
      if (row->VirtualKey == 0) {
        break;
      }
      const auto chars = reinterpret_cast<const WCHAR*>(
        bytes + offsetof(VK_TO_WCHARS1, wch));
      table.mRows.push_back({
        .mVK = row->VirtualKey,
        .mAttributes = row->Attributes,
        .mChars = std::vector<wchar_t>(chars, chars + mods),
      });
    }
    // Like Windows, only the first row for a key in each table counts. The
    // row after an SGCAPS row holds its CapsLock characters, and often
    // repeats the VK (e.g. Swiss German) rather than using 0xFF.
    const auto tableIndex = static_cast<uint16_t>(impl.mTables.size());
    for (std::size_t r = 0; r < table.mRows.size(); ++r) {
      const auto& row = table.mRows[r];
      if (row.mVK != AttachedRowVK) {
        auto& refs = impl.mRowsForVK[row.mVK];
        if (refs.empty() || refs.back().mTable != tableIndex) {
          refs.push_back({tableIndex, static_cast<uint16_t>(r)});
        }
      }
      if (row.mAttributes & SGCAPS) {
        ++r;
      }
    }
    impl.mTables.push_back(std::move(table));
  }

  // Dead keys
  if (const auto deadKeys = tables.pDeadKey) {
    for (std::size_t i = 0; i < MaxDeadKeys && deadKeys[i].dwBoth; ++i) {
      // First entry wins, like a linear search would
      impl.mDeadKeys.try_emplace(
        deadKeys[i].dwBoth,
        Layout::Impl::DeadKeyResult {
          .mComposed = deadKeys[i].wchComposed,
          .mChained = (deadKeys[i].uFlags & DKF_DEAD) != 0,
        });
    }
  }

  // Ligatures
  if (tables.pLigature && tables.nLgMax > 0) {
    const auto charsOffset = offsetof(LIGATURE1, wch);
    if (
      tables.cbLgEntry
      >= charsOffset + (static_cast<std::size_t>(tables.nLgMax) * sizeof(WCHAR))) {
      auto bytes = reinterpret_cast<const std::byte*>(tables.pLigature);
      for (std::size_t i = 0; i < MaxLigatures; ++i, bytes += tables.cbLgEntry) {
        const auto entry = reinterpret_cast<const LIGATURE1*>(bytes);
        if (entry->VirtualKey == 0) {
          break;
        }
        const auto chars
          = reinterpret_cast<const WCHAR*>(bytes + charsOffset);
        std::wstring text;
        for (std::size_t c = 0; c < tables.nLgMax; ++c) {
          if (chars[c] == WCH_NONE || chars[c] == 0) {
            break;
          }
          text.push_back(chars[c]);
        }
        impl.mLigatures.try_emplace(
          (static_cast<uint32_t>(entry->VirtualKey) << 16)
            | entry->ModificationNumber,
          std::move(text));
      }
    }
  }

  // Scan codes
  if (!tables.pusVSCtoVK) {
    return false;
  }
  impl.mScanCodeToVK.assign(
    tables.pusVSCtoVK, tables.pusVSCtoVK + tables.bMaxVSCtoVK);
  const auto copyExtended
    = [](const VSC_VK* source, std::vector<std::pair<uint8_t, uint16_t>>& dest) {
        if (!source) {
          return;
        }
        for (std::size_t i = 0; i < MaxExtendedScanCodes && source[i].Vsc;
             ++i) {
          dest.emplace_back(source[i].Vsc, source[i].Vk);
        }
      };
  copyExtended(tables.pVSCtoVK_E0, impl.mE0ScanCodeToVK);
  copyExtended(tables.pVSCtoVK_E1, impl.mE1ScanCodeToVK);

  return true;
}

using KbdLayerDescriptorFn = PKBDTABLES(WINAPI*)();

bool CopyTablesFromDLL(KbdLayerDescriptorFn descriptor, Layout::Impl* impl) {
  __try {
    const auto tables = descriptor();
    if (!tables) {
      return false;
    }
    return CopyTables(*tables, *impl);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}// namespace

uint16_t Layout::Impl::GetFlaggedVirtualKey(ScanCode scanCode) const noexcept {
  const auto prefix = scanCode & 0xff00;
  const auto code = static_cast<uint8_t>(scanCode & 0xff);
  const auto search = [code](const auto& table) -> uint16_t {
    for (const auto& [vsc, vk]: table) {
      if (vsc == code) {
        return vk;
      }
    }
    return 0;
  };
  switch (prefix) {
    case 0:
      return code < mScanCodeToVK.size() ? mScanCodeToVK[code] : 0;
    case 0xe000:
      return search(mE0ScanCodeToVK);
    case 0xe100:
      return search(mE1ScanCodeToVK);
    default:
      return 0;
  }
}

std::optional<uint8_t> Layout::Impl::GetModificationNumber(
  uint32_t modBits,
  const Table& table) const noexcept {
  if (modBits >= mModNumbers.size()) {
    return std::nullopt;
  }
  const auto ret = mModNumbers[modBits];
  if (ret == SHFT_INVALID || ret >= table.mModifications) {
    return std::nullopt;
  }
  return ret;
}

Layout::Impl::Lookup Layout::Impl::LookUp(
  uint8_t vk,
  const ModifierState& state) const {
  uint32_t modBits = 0;
  if (state.mShift) {
    modBits |= mShiftBit;
  }
  if (state.mAltGr) {
    modBits |= mCtrlBit | mAltBit;
  }
  const auto capsAttribute = state.mAltGr ? CAPLOKALTGR : CAPLOK;

  for (const auto& ref: mRowsForVK[vk]) {
    const auto& table = mTables[ref.mTable];
    std::size_t rowIndex = ref.mRow;
    auto keyModBits = modBits;
    if (state.mCapsLock) {
      const auto attributes = table.mRows[rowIndex].mAttributes;
      if (
        (attributes & SGCAPS) && !state.mAltGr
        && rowIndex + 1 < table.mRows.size()) {
        // Swiss German style: the next row has the CapsLock characters for
        // the unshifted and shifted states. It can also be the dead key row
        // (e.g. Czech), so we take its VK as-is.
        ++rowIndex;
      } else if (attributes & capsAttribute) {
        // CapsLock acts like Shift, so Shift+CapsLock gives the base
        // character
        keyModBits ^= mShiftBit;
      }
    }
    const auto modification = GetModificationNumber(keyModBits, table);
    if (!modification) {
      continue;
    }
    const auto c = table.mRows[rowIndex].mChars[*modification];
    if (c == WCH_NONE) {
      continue;
    }
    Lookup ret {.mChar = c, .mVK = vk, .mModification = *modification};
    if (c == WCH_DEAD) {
      // The dead character is in the next row; that's usually VK 0xFF, but
      // Windows doesn't check
      if (rowIndex + 1 >= table.mRows.size()) {
        return {};
      }
      ret.mDeadChar = table.mRows[rowIndex + 1].mChars[*modification];
      if (ret.mDeadChar == WCH_NONE || ret.mDeadChar == 0) {
        return {};
      }
    }
    return ret;
  }
  return {};
}

KeyOutput Layout::Impl::GetOutput(
  ScanCode scanCode,
  const ModifierState& state) const {
  const auto flagged = GetFlaggedVirtualKey(scanCode);
  auto vk = static_cast<uint8_t>(flagged & 0xff);
  if (flagged & KBDNUMPAD) {
    for (const auto& [from, to]: NumpadVKs) {
      if (from == vk) {
        vk = to;
        break;
      }
    }
  }
  if (vk == 0 || vk == 0xff) {
    return {};
  }

  const auto lookup = LookUp(vk, state);
  switch (lookup.mChar) {
    case WCH_NONE:
      return {};
    case WCH_DEAD:
      return {KeyOutput::Kind::DeadKey, std::wstring(1, lookup.mDeadChar)};
    case WCH_LGTR: {
      const auto it = mLigatures.find(
        (static_cast<uint32_t>(lookup.mVK) << 16) | lookup.mModification);
      if (it == mLigatures.end() || it->second.empty()) {
        return {};
      }
      return {KeyOutput::Kind::Text, it->second};
    }
    default:
      if (lookup.mChar == 0 || IsControlCharacter(lookup.mChar)) {
        return {};
      }
      return {KeyOutput::Kind::Text, std::wstring(1, lookup.mChar)};
  }
}

Layout::Layout(std::unique_ptr<Impl> impl) : p(std::move(impl)) {
}

Layout::~Layout() = default;

std::shared_ptr<const Layout> Layout::Load(std::string_view klid) {
  auto info = GetLayoutInfo(klid);
  if (!info || !IsPlainFileName(info->mLayoutFile)) {
    return nullptr;
  }

  const auto module = LoadLibraryExW(
    info->mLayoutFile.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) {
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->mInfo = std::move(*info);
  const auto descriptor = reinterpret_cast<KbdLayerDescriptorFn>(
    reinterpret_cast<void*>(GetProcAddress(module, "KbdLayerDescriptor")));
  // Everything is copied, so the DLL doesn't need to stay loaded
  const bool copied = descriptor && CopyTablesFromDLL(descriptor, impl.get());
  FreeLibrary(module);
  if (!copied) {
    return nullptr;
  }

  // The ISO key is between left Shift and Z. ANSI-only layouts (e.g. US)
  // often map it to a second backslash key, so only count it if it's
  // different from the ANSI backslash key (0x2B; '#' on ISO keyboards).
  constexpr ScanCode IsoKey = 0x56;
  constexpr ScanCode BackslashKey = 0x2b;
  bool isoProducesSomething = false;
  bool isoMatchesBackslash = true;
  for (const auto shift: {false, true}) {
    const ModifierState state {.mShift = shift};
    const auto iso = impl->GetOutput(IsoKey, state);
    if (iso.mKind != KeyOutput::Kind::None) {
      isoProducesSomething = true;
    }
    if (iso != impl->GetOutput(BackslashKey, state)) {
      isoMatchesBackslash = false;
    }
  }
  impl->mIsISO = isoProducesSomething && !isoMatchesBackslash;

  return std::shared_ptr<const Layout>(new Layout(std::move(impl)));
}

const LayoutInfo& Layout::GetInfo() const noexcept {
  return p->mInfo;
}

bool Layout::HasAltGr() const noexcept {
  return (p->mLocaleFlags & KLLF_ALTGR) != 0;
}

bool Layout::IsISO() const noexcept {
  return p->mIsISO;
}

uint8_t Layout::GetVirtualKey(ScanCode scanCode) const noexcept {
  const auto vk = static_cast<uint8_t>(p->GetFlaggedVirtualKey(scanCode) & 0xff);
  return vk == 0xff ? 0 : vk;
}

uint8_t Layout::GetNumLockVirtualKey(ScanCode scanCode) const noexcept {
  const auto flagged = p->GetFlaggedVirtualKey(scanCode);
  const auto vk = static_cast<uint8_t>(flagged & 0xff);
  if (flagged & KBDNUMPAD) {
    for (const auto& [from, to]: NumpadVKs) {
      if (from == vk) {
        return to;
      }
    }
  }
  return vk == 0xff ? 0 : vk;
}

bool Layout::IsNumpadKey(ScanCode scanCode) const noexcept {
  return (p->GetFlaggedVirtualKey(scanCode) & KBDNUMPAD) != 0;
}

std::optional<ScanCode> Layout::GetScanCode(uint8_t vk) const noexcept {
  if (vk == 0 || vk == 0xff) {
    return std::nullopt;
  }
  const auto& base = p->mScanCodeToVK;

  // NumLock-on numpad keys
  for (const auto& [from, to]: NumpadVKs) {
    if (to != vk) {
      continue;
    }
    for (std::size_t i = 0; i < base.size(); ++i) {
      if ((base[i] & KBDNUMPAD) && (base[i] & 0xff) == from) {
        return static_cast<ScanCode>(i);
      }
    }
    return std::nullopt;
  }

  // Dedicated keys first, then the numpad (e.g. VK_HOME is 0xE047 rather
  // than numpad 7)
  for (std::size_t i = 0; i < base.size(); ++i) {
    if (!(base[i] & KBDNUMPAD) && (base[i] & 0xff) == vk) {
      return static_cast<ScanCode>(i);
    }
  }
  for (const auto& [vsc, flagged]: p->mE0ScanCodeToVK) {
    if ((flagged & 0xff) == vk) {
      return static_cast<ScanCode>(0xe000 | vsc);
    }
  }
  for (std::size_t i = 0; i < base.size(); ++i) {
    if ((base[i] & 0xff) == vk) {
      return static_cast<ScanCode>(i);
    }
  }
  for (const auto& [vsc, flagged]: p->mE1ScanCodeToVK) {
    if ((flagged & 0xff) == vk) {
      return static_cast<ScanCode>(0xe100 | vsc);
    }
  }

  // The tables only have the left/right versions of the modifiers
  switch (vk) {
    case VK_SHIFT:
      return GetScanCode(VK_LSHIFT);
    case VK_CONTROL:
      return GetScanCode(VK_LCONTROL);
    case VK_MENU:
      return GetScanCode(VK_LMENU);
    default:
      return std::nullopt;
  }
}

KeyOutput Layout::GetOutput(ScanCode scanCode, const ModifierState& state)
  const {
  return p->GetOutput(scanCode, state);
}

std::optional<wchar_t> Layout::Compose(wchar_t deadChar, wchar_t baseChar)
  const {
  const auto it = p->mDeadKeys.find(DeadKeyID(deadChar, baseChar));
  if (it == p->mDeadKeys.end()) {
    return std::nullopt;
  }
  return it->second.mComposed;
}

bool Layout::IsChainedDeadKey(wchar_t deadChar, wchar_t baseChar) const {
  const auto it = p->mDeadKeys.find(DeadKeyID(deadChar, baseChar));
  return it != p->mDeadKeys.end() && it->second.mChained;
}

wchar_t Layout::GetSpacingDeadChar(wchar_t deadChar) const {
  const auto it = p->mDeadKeys.find(DeadKeyID(deadChar, L' '));
  if (
    it == p->mDeadKeys.end() || it->second.mChained
    || it->second.mComposed == 0 || IsControlCharacter(it->second.mComposed)) {
    return deadChar;
  }
  return it->second.mComposed;
}

////////////////////////////////////////////////////////////////////////////////
// Typist
////////////////////////////////////////////////////////////////////////////////

Typist::Typist(std::shared_ptr<const Layout> layout)
  : mLayout(std::move(layout)) {
}

Typist::Result Typist::Press(ScanCode scanCode, const ModifierState& state) {
  if (!mLayout) {
    return {};
  }

  switch (mLayout->GetVirtualKey(scanCode)) {
    case VK_BACK:
    case VK_RETURN:
    case VK_TAB:
    case VK_ESCAPE:
      mPending.reset();
      return {};
    default:
      break;
  }

  const auto output = mLayout->GetOutput(scanCode, state);
  if (output.mKind == KeyOutput::Kind::None || output.mText.empty()) {
    return {.mPendingDeadKey = mPending};
  }

  if (!mPending) {
    if (output.mKind == KeyOutput::Kind::DeadKey) {
      mPending = output.mText.front();
      return {.mPendingDeadKey = mPending};
    }
    return {.mText = output.mText};
  }

  const auto accent = *mPending;
  mPending.reset();

  // Ligatures never compose
  if (output.mText.size() == 1) {
    const auto base = output.mText.front();
    if (const auto composed = mLayout->Compose(accent, base)) {
      if (mLayout->IsChainedDeadKey(accent, base)) {
        mPending = *composed;
        return {.mPendingDeadKey = mPending};
      }
      return {.mText = std::wstring(1, *composed)};
    }
    if (output.mKind == KeyOutput::Kind::Text && base == L' ') {
      return {.mText = std::wstring(1, accent)};
    }
  }

  // Not composable: the accent, then whatever the key types on its own. A
  // second dead key types its own accent rather than becoming pending.
  return {.mText = accent + output.mText};
}

void Typist::CancelDeadKey() {
  mPending.reset();
}

std::optional<wchar_t> Typist::GetPendingDeadKey() const {
  return mPending;
}

const std::shared_ptr<const Layout>& Typist::GetLayout() const {
  return mLayout;
}

}// namespace OpenDesktopVR::KeyboardLayout
