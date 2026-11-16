// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Import tables are read as the PE/COFF specification and the Windows SDK's
// winnt.h describe them (IMAGE_IMPORT_DESCRIPTOR, IMAGE_DELAYLOAD_DESCRIPTOR).

#include <OpenDesktopVR/ImportPatch.hpp>

#include <Psapi.h>

#include <algorithm>
#include <cstring>

namespace OpenDesktopVR::AppHelper {

namespace {

constexpr DWORD ExecutablePages = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

// user32 itself, or an API set that forwards to it
bool IsUser32(const char* dll) noexcept {
  return _stricmp(dll, "user32.dll") == 0 || _strnicmp(dll, "api-ms-win-ntuser-", 18) == 0
    || _strnicmp(dll, "ext-ms-win-ntuser-", 18) == 0;
}

bool InImage(DWORD imageSize, DWORD rva, DWORD size) noexcept {
  return rva != 0 && rva < imageSize && size <= imageSize - rva;
}

bool NameIs(const BYTE* base, const IMAGE_THUNK_DATA& name, const char* functionName) noexcept {
  if (IMAGE_SNAP_BY_ORDINAL(name.u1.Ordinal)) {
    return false;
  }
  const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name.u1.AddressOfData);
  return std::strcmp(reinterpret_cast<const char*>(byName->Name), functionName) == 0;
}

// Plain C-style, so structured exception handling can guard it: a module
// with odd tables is skipped, never a crash in the app
void Walk(ImportPatch& patch, HMODULE module, const char* functionName, void* original) noexcept {
  __try {
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
      return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
      return;
    }
    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;

    // Regular imports: by address (resolved), or by name from user32
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (InImage(imageSize, imports.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
      for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
           d->Name != 0 && d->FirstThunk != 0; ++d) {
        const bool fromUser32 = IsUser32(reinterpret_cast<const char*>(base + d->Name));
        auto* table = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        const auto* names = d->OriginalFirstThunk != 0
          ? reinterpret_cast<const IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk)
          : nullptr;
        for (std::size_t i = 0; table[i].u1.Function != 0; ++i) {
          auto** slot = reinterpret_cast<void**>(&table[i].u1.Function);
          if (*slot == original || (fromUser32 && names && NameIs(base, names[i], functionName))) {
            patch.Patch(module, slot);
          }
        }
      }
    }

    // Delay-load imports (Chromium delay-loads user32): by name. A slot not
    // yet resolved points at the loader's stub; it never will be now.
    const auto& delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (InImage(imageSize, delayed.VirtualAddress, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR))) {
      for (auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayed.VirtualAddress);
           d->DllNameRVA != 0; ++d) {
        if (!d->Attributes.RvaBased || !IsUser32(reinterpret_cast<const char*>(base + d->DllNameRVA))) {
          continue;
        }
        auto* table = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA);
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA);
        for (std::size_t i = 0; names[i].u1.AddressOfData != 0; ++i) {
          if (NameIs(base, names[i], functionName)) {
            patch.Patch(module, reinterpret_cast<void**>(&table[i].u1.Function));
          }
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

bool IsSystemModule(HMODULE module) noexcept {
  wchar_t windows[MAX_PATH] {};
  const UINT windowsLength = GetSystemWindowsDirectoryW(windows, MAX_PATH);
  wchar_t path[MAX_PATH * 2] {};
  const DWORD pathLength = GetModuleFileNameW(module, path, MAX_PATH * 2);
  if (windowsLength == 0 || windowsLength >= MAX_PATH || pathLength == 0 || pathLength >= MAX_PATH * 2) {
    return true;// can't tell: leave it alone
  }
  return pathLength > windowsLength && path[windowsLength] == L'\\'
    && CompareStringOrdinal(path, static_cast<int>(windowsLength), windows, static_cast<int>(windowsLength), TRUE) == CSTR_EQUAL;
}

// Swaps a slot that holds `expected`, making its page writable meanwhile
bool Swap(void** slot, void* expected, void* desired) noexcept {
  MEMORY_BASIC_INFORMATION info {};
  if (VirtualQuery(slot, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
    return false;
  }
  const DWORD writable = (info.Protect & ExecutablePages) != 0 ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
  DWORD previous = 0;
  if (!VirtualProtect(slot, sizeof(void*), writable, &previous)) {
    return false;
  }
  const bool swapped = InterlockedCompareExchangePointer(slot, desired, expected) == expected;
  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(void*), previous, &ignored);
  return swapped;
}

}// namespace

std::size_t ImportPatch::Apply(const char* functionName, void* original, void* replacement, HMODULE skip) noexcept {
  if (!functionName || !original || !replacement || (mReplacement && mReplacement != replacement)) {
    return Count();
  }
  mReplacement = replacement;
  HMODULE modules[1024] {};
  DWORD needed = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
    return Count();
  }
  const std::size_t found = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(modules));
  for (std::size_t i = 0; i < found; ++i) {
    if (modules[i] == skip || IsSystemModule(modules[i])) {
      continue;
    }
    // Held while it's walked, so it can't be unloaded meanwhile
    HMODULE held = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(modules[i]), &held)) {
      continue;
    }
    Walk(*this, held, functionName, original);
    FreeLibrary(held);
  }
  return Count();
}

void ImportPatch::Patch(HMODULE module, void** slot) noexcept {
  const auto count = mCount.load(std::memory_order_relaxed);
  if (count >= MaxSlots || Has(slot)) {
    return;
  }
  void* current = *const_cast<void* volatile*>(slot);
  if (current == mReplacement || current == nullptr) {
    return;
  }
  if (Swap(slot, current, mReplacement)) {
    mSlots[count] = {module, slot, current};
    mCount.store(count + 1, std::memory_order_release);
  }
}

bool ImportPatch::Has(void** slot) const noexcept {
  const auto count = mCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; ++i) {
    if (mSlots[i].mAddress == slot) {
      return true;
    }
  }
  return false;
}

std::size_t ImportPatch::Restore() noexcept {
  std::size_t restored = 0;
  const auto count = mCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& slot = mSlots[i];
    // Only if its module is still the one loaded there (held meanwhile)
    HMODULE held = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(slot.mAddress), &held)) {
      continue;
    }
    if (held == slot.mModule && Swap(slot.mAddress, mReplacement, slot.mOriginal)) {
      ++restored;
    }
    FreeLibrary(held);
  }
  mCount.store(0, std::memory_order_release);
  return restored;
}

}// namespace OpenDesktopVR::AppHelper
