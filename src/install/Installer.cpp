// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Installer.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/Version.hpp>

#include <Windows.h>

#include <AclAPI.h>
#include <KnownFolders.h>
#include <RestartManager.h>
#include <ShlObj.h>
#include <sddl.h>
#include <wrl/client.h>

#include <algorithm>
#include <ctime>
#include <format>
#include <set>
#include <thread>

namespace OpenDesktopVR::Install {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t RealUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OpenDesktopVR";
constexpr wchar_t RealLayerKey[] = L"Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
// "ALL APPLICATION PACKAGES"
constexpr wchar_t AppPackagesSid[] = L"S-1-15-2-1";

// What the test builds' zip (install.cmd, before M7) put in the folder that
// a setup install doesn't: removed when the folder has no manifest yet
constexpr std::string_view TestBuildLeftovers[] = {
  "install.cmd",
  "uninstall.cmd",
  "command-prompt.cmd",
  "tools/openxr-simulator.exe",
  "tools/click-target.exe",
  "tools/self-test.cmd",
};

// The files this program ships: what an uninstall removes when the manifest
// can't be read
std::vector<std::string> ShippedFiles() {
  return {
    StringUtil::Narrow(HostFileName),
    StringUtil::Narrow(Product::LayerDllName),
    StringUtil::Narrow(Product::LayerManifestName),
    StringUtil::Narrow(Product::AppHelperDllName),
    StringUtil::Narrow(SetupFileName),
    "odvr-layer.exe",
    "BUILD-INFO.txt",
    "LICENSE",
    "THIRD_PARTY_NOTICES",
    "licenses/Apache-2.0.txt",
    "tools/odvr-mirror.exe",
  };
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path) {
    result = path;
  }
  CoTaskMemFree(path);
  return result;
}

fs::path Under(const fs::path& folder, std::string_view relative) {
  fs::path part {StringUtil::Widen(relative)};
  return folder / part.make_preferred();
}

std::string Lower(std::string_view text) {
  std::string lower;
  for (const char c: text) {
    lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  return lower;
}

std::optional<std::vector<std::string>> ReadManifest(const fs::path& folder) {
  const auto text = FileUtil::ReadAll(folder / ManifestFileName);
  if (!text) {
    return std::nullopt;
  }
  const auto json = JsonUtil::Parse(*text);
  const auto files = json ? JsonUtil::Get<JsonUtil::Json>(*json, "files") : std::nullopt;
  if (!files || !files->is_array()) {
    return std::nullopt;
  }
  // Untrusted (TH-9): only paths that stay inside the folder
  std::vector<std::string> list;
  for (const auto& entry: *files) {
    if (entry.is_string() && list.size() < MaxPayloadFiles) {
      auto path = entry.get<std::string>();
      if (IsSafeRelativePath(path)) {
        list.push_back(std::move(path));
      }
    }
  }
  return list;
}

bool WriteManifest(const fs::path& folder, const std::string& version, const std::vector<std::string>& files) {
  JsonUtil::Json json = JsonUtil::Json::object();
  json["version"] = version;
  json["files"] = JsonUtil::Json::array();
  for (const auto& file: files) {
    json["files"].push_back(file);
  }
  const auto result = FileUtil::WriteAtomic(folder / ManifestFileName, JsonUtil::Dump(json));
  return result.mOutcome != FileUtil::WriteOutcome::Failed;
}

bool WriteWhole(const fs::path& path, std::string_view data) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool ok = true;
  std::size_t done = 0;
  while (ok && done < data.size()) {
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 20));
    DWORD written = 0;
    ok = WriteFile(file, data.data() + done, chunk, &written, nullptr) && written == chunk;
    done += written;
  }
  ok = ok && FlushFileBuffers(file);
  CloseHandle(file);
  return ok;
}

bool IsInUseError(DWORD error) noexcept {
  return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
    || error == ERROR_USER_MAPPED_FILE;
}

// Renames a file in use to "<name>.odvr-old-<n>": Windows allows that for a
// loaded DLL or a running program, which keep running from it
bool SetAside(const fs::path& target) {
  for (int n = 1; n < 100; ++n) {
    const fs::path aside = std::format(L"{}{}-{}", target.native(), AsideSuffix, n);
    if (MoveFileExW(target.c_str(), aside.c_str(), 0)) {
      return true;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
      return false;
    }
  }
  return false;
}

// Puts `fresh` in `target`'s place, setting `target` aside if it's in use
bool Replace(const fs::path& fresh, const fs::path& target, std::size_t& setAside) {
  for (int attempt = 0; attempt < 6; ++attempt) {
    if (MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
    if (IsInUseError(GetLastError()) && SetAside(target)) {
      ++setAside;
      continue;
    }
    // A virus scanner's or indexer's brief hold (ST-2)
    std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
  }
  return false;
}

// Deletes the file, or sets it aside if it's in use; true if it's gone from
// its name
bool RemoveOrSetAside(const fs::path& path, std::size_t& inUse) {
  if (DeleteFileW(path.c_str())) {
    return true;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (IsInUseError(error) && SetAside(path)) {
    ++inUse;
    return true;
  }
  return false;
}

// Empty subfolders, deepest first; then `folder` itself if asked
void RemoveEmptyFolders(const fs::path& folder, bool andFolder) {
  std::error_code error;
  std::vector<fs::path> folders;
  for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, error);
       !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
    if (it->is_directory(error)) {
      folders.push_back(it->path());
    }
  }
  std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) {
    return a.native().size() > b.native().size();
  });
  for (const auto& sub: folders) {
    RemoveDirectoryW(sub.c_str());// only succeeds if empty
  }
  if (andFolder) {
    RemoveDirectoryW(folder.c_str());
  }
}

bool SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))
    == ERROR_SUCCESS;
}

bool SetDword(HKEY key, const wchar_t* name, DWORD value) {
  return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

std::optional<std::wstring> GetString(const std::wstring& key, const wchar_t* name) {
  wchar_t buffer[1024] {};
  DWORD size = sizeof(buffer) - sizeof(wchar_t);
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size)
      != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return std::wstring {buffer};
}

// A Restart Manager session, ended when it goes
class RmSession {
 public:
  RmSession() {
    wchar_t key[CCH_RM_SESSION_KEY + 1] {};
    mOpen = RmStartSession(&mHandle, 0, key) == ERROR_SUCCESS;
  }
  ~RmSession() {
    if (mOpen) {
      RmEndSession(mHandle);
    }
  }
  RmSession(const RmSession&) = delete;
  RmSession& operator=(const RmSession&) = delete;

  bool Register(std::span<const fs::path> files) {
    std::vector<LPCWSTR> names;
    for (const auto& file: files) {
      if (FileUtil::Exists(file)) {
        names.push_back(file.c_str());
      }
    }
    return mOpen && !names.empty()
      && RmRegisterResources(mHandle, static_cast<UINT>(names.size()), names.data(), 0, nullptr, 0, nullptr)
      == ERROR_SUCCESS;
  }

  std::vector<RM_PROCESS_INFO> List() {
    std::vector<RM_PROCESS_INFO> infos;
    for (int attempt = 0; mOpen && attempt < 4; ++attempt) {
      UINT needed = 0;
      UINT count = static_cast<UINT>(infos.size());
      DWORD reasons = 0;
      const DWORD result = RmGetList(mHandle, &needed, &count, infos.empty() ? nullptr : infos.data(), &reasons);
      if (result == ERROR_SUCCESS) {
        infos.resize(count);
        return infos;
      }
      if (result != ERROR_MORE_DATA) {
        break;
      }
      infos.resize(needed + 4);// processes may start between the calls
    }
    return {};
  }

  bool Shutdown() {
    return mOpen && RmShutdown(mHandle, 0, nullptr) == ERROR_SUCCESS;
  }

 private:
  DWORD mHandle {0};
  bool mOpen {false};
};

}// namespace

Layout DefaultLayout() {
  Layout layout;
  layout.mDataFolder = Product::DataFolder();
  const auto instance = ProcessUtil::GetEnvironment("OPENDESKTOPVR_INSTANCE");
  const auto root = ProcessUtil::GetEnvironment("OPENDESKTOPVR_INSTALL_ROOT");
  if (instance && !instance->empty() && root && !root->empty()) {
    const fs::path base {StringUtil::Widen(*root)};
    const auto key = L"Software\\OpenDesktopVR-Tests\\" + StringUtil::Widen(*instance);
    layout.mInstallFolder = base / L"Programs" / Product::FolderName;
    layout.mShortcut = base / L"Start Menu" / (std::wstring {Product::DisplayName} + L".lnk");
    layout.mUninstallKey = key + L"\\Uninstall\\OpenDesktopVR";
    layout.mLayerKey = key + L"\\ApiLayers\\Implicit";
    layout.mTest = true;
    return layout;
  }
  // %LOCALAPPDATA%\Programs\OpenDesktopVR (ADR-009)
  layout.mInstallFolder = KnownFolder(FOLDERID_UserProgramFiles) / Product::FolderName;
  layout.mShortcut = KnownFolder(FOLDERID_Programs) / (std::wstring {Product::DisplayName} + L".lnk");
  layout.mUninstallKey = RealUninstallKey;
  layout.mLayerKey = RealLayerKey;
  return layout;
}

std::optional<Installed> FindInstalled(const Layout& layout) noexcept {
  try {
    if (auto version = GetString(layout.mUninstallKey, L"DisplayVersion")) {
      Installed installed;
      installed.mVersion = StringUtil::Narrow(*version);
      const auto folder = GetString(layout.mUninstallKey, L"InstallLocation");
      installed.mFolder = folder && !folder->empty() ? fs::path {*folder} : layout.mInstallFolder;
      return installed;
    }
    // A test build put there by install.cmd: no entry, but the program
    if (FileUtil::Exists(layout.mInstallFolder / HostFileName)) {
      return Installed {"", layout.mInstallFolder};
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::wstring HostArguments(std::string_view previous, UpdatedBy by) {
  if (!ParseVersion(previous)) {
    return {};
  }
  std::wstring arguments = L"--updated-from " + StringUtil::Widen(previous);
  if (by == UpdatedBy::Itself) {
    arguments += L" --updated-by itself";
  } else if (by == UpdatedBy::Request) {
    arguments += L" --updated-by request";
  }
  return arguments;
}

UpdatedBy ParseUpdatedBy(std::wstring_view text) noexcept {
  if (text == L"itself") {
    return UpdatedBy::Itself;
  }
  if (text == L"request") {
    return UpdatedBy::Request;
  }
  return UpdatedBy::Setup;
}

WriteReport WriteFiles(
  const Payload& payload, const fs::path& folder, std::span<const std::string> extra, const Progress& progress) noexcept {
  WriteReport report;
  try {
    std::error_code error;
    fs::create_directories(folder, error);
    if (!fs::is_directory(folder, error)) {
      report.mError = std::format("couldn't make the folder {}", StringUtil::Narrow(folder.wstring()));
      return report;
    }
    CleanAside(folder);
    const auto previous = ReadManifest(folder);
    // First every new file beside its target, where failures happen (a full
    // disk, no access): if one fails, they all go and nothing has changed.
    // Then they're swapped in, which is only renames.
    std::vector<std::pair<fs::path, fs::path>> swaps;// fresh, target
    const auto discard = [&swaps] {
      for (const auto& [fresh, target]: swaps) {
        DeleteFileW(fresh.c_str());
      }
    };
    for (const auto& file: payload.mFiles) {
      if (!IsSafeRelativePath(file.mPath)) {
        discard();
        report.mError = std::format("'{}' isn't a safe path", file.mPath);
        return report;
      }
      const auto target = Under(folder, file.mPath);
      fs::create_directories(target.parent_path(), error);
      fs::path fresh = target.native() + std::wstring {NewSuffix};
      const bool written = WriteWhole(fresh, file.mData);
      const DWORD failure = GetLastError();
      swaps.emplace_back(std::move(fresh), target);
      if (!written) {
        discard();
        report.mError = std::format("couldn't write {} (error {}); nothing was changed", file.mPath, failure);
        return report;
      }
      if (progress) {
        progress(swaps.size(), payload.mFiles.size());
      }
    }
    for (std::size_t i = 0; i < swaps.size(); ++i) {
      const auto& [fresh, target] = swaps[i];
      if (!Replace(fresh, target, report.mSetAside)) {
        const DWORD failure = GetLastError();
        for (std::size_t j = i; j < swaps.size(); ++j) {
          DeleteFileW(swaps[j].first.c_str());
        }
        report.mError = std::format("couldn't replace {} (error {}); run setup again to finish",
          StringUtil::Narrow(target.filename().wstring()), failure);
        return report;
      }
      ++report.mWritten;
    }
    std::vector<std::string> listed;
    std::set<std::string> now;
    for (const auto& file: payload.mFiles) {
      listed.push_back(file.mPath);
      now.insert(Lower(file.mPath));
    }
    for (const auto& file: extra) {
      if (IsSafeRelativePath(file) && now.insert(Lower(file)).second) {
        listed.push_back(file);
      }
    }
    // What the last install had that this one doesn't
    std::vector<std::string> stale;
    if (previous) {
      for (const auto& file: *previous) {
        if (!now.contains(Lower(file))) {
          stale.push_back(file);
        }
      }
    } else {
      stale.assign(std::begin(TestBuildLeftovers), std::end(TestBuildLeftovers));
    }
    std::size_t inUse = 0;
    for (const auto& file: stale) {
      const auto path = Under(folder, file);
      if (FileUtil::Exists(path) && RemoveOrSetAside(path, inUse)) {
        ++report.mRemoved;
      }
    }
    if (!WriteManifest(folder, payload.mVersion, listed)) {
      report.mError = "couldn't write the list of installed files";
      return report;
    }
    RemoveEmptyFolders(folder, false);
    report.mOk = true;
  } catch (...) {
    report.mError = "unexpected: " + ThreadUtil::DescribeCurrentException();
  }
  return report;
}

std::size_t CleanAside(const fs::path& folder) noexcept {
  std::size_t left = 0;
  try {
    std::error_code error;
    std::vector<fs::path> folders {folder};
    for (const auto& entry: fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, error)) {
      if (entry.is_directory(error)) {
        folders.push_back(entry.path());
      }
    }
    for (const auto& place: folders) {
      for (const auto& entry: fs::directory_iterator(place, fs::directory_options::skip_permission_denied, error)) {
        const auto name = entry.path().filename().native();
        const bool aside = name.find(AsideSuffix) != std::wstring::npos;
        if ((aside || name.ends_with(NewSuffix)) && !entry.is_directory(error)) {
          if (!DeleteFileW(entry.path().c_str()) && aside) {
            ++left;
          }
        }
      }
    }
  } catch (...) {
  }
  return left;
}

RemoveReport RemoveFiles(const fs::path& folder) noexcept {
  RemoveReport report;
  try {
    auto files = ReadManifest(folder).value_or(ShippedFiles());
    files.insert(files.end(), std::begin(TestBuildLeftovers), std::end(TestBuildLeftovers));
    for (const auto& file: files) {
      const auto path = Under(folder, file);
      if (FileUtil::Exists(path) && RemoveOrSetAside(path, report.mInUse)) {
        ++report.mRemoved;
      }
    }
    report.mInUse = CleanAside(folder);
    DeleteFileW((folder / ManifestFileName).c_str());
    RemoveEmptyFolders(folder, true);
    std::error_code error;
    report.mOk = !fs::exists(folder, error);
  } catch (...) {
  }
  return report;
}

bool PlaceFile(const fs::path& source, const fs::path& target) noexcept {
  try {
    std::error_code error;
    if (fs::equivalent(source, target, error)) {
      return true;// running from there already (a repair from Apps and Features)
    }
    const fs::path fresh = target.native() + std::wstring {NewSuffix};
    if (!CopyFileW(source.c_str(), fresh.c_str(), FALSE)) {
      return false;
    }
    std::size_t setAside = 0;
    if (!Replace(fresh, target, setAside)) {
      DeleteFileW(fresh.c_str());
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool WriteShortcut(const fs::path& shortcut, const fs::path& target, std::wstring_view description) noexcept {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool ok = false;
  try {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    std::error_code error;
    fs::create_directories(shortcut.parent_path(), error);
    const std::wstring text {description};
    ok = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))
      && SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link->SetWorkingDirectory(target.parent_path().c_str()))
      && SUCCEEDED(link->SetDescription(text.c_str())) && SUCCEEDED(link->SetIconLocation(target.c_str(), 0))
      && SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
  } catch (...) {
    ok = false;
  }
  if (SUCCEEDED(init)) {
    CoUninitialize();
  }
  return ok;
}

bool RemoveShortcut(const fs::path& shortcut) noexcept {
  return DeleteFileW(shortcut.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}

bool WriteUninstallEntry(const Layout& layout, const UninstallEntry& entry) noexcept {
  try {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, layout.mUninstallKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
          nullptr, &key, nullptr)
        != ERROR_SUCCESS) {
      return false;
    }
    const auto setup = L"\"" + entry.mSetup.wstring() + L"\"";
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    const auto date = std::format(L"{:04}{:02}{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    const bool ok = SetString(key, L"DisplayName", std::wstring {Product::DisplayName})
      && SetString(key, L"DisplayVersion", StringUtil::Widen(entry.mVersion))
      && SetString(key, L"Publisher", L"Robert McGough")
      && SetString(key, L"InstallLocation", entry.mFolder.wstring())
      && SetString(key, L"DisplayIcon", (entry.mFolder / HostFileName).wstring() + L",0")
      && SetString(key, L"UninstallString", setup + L" --uninstall")
      && SetString(key, L"QuietUninstallString", setup + L" --uninstall --silent") && SetString(key, L"InstallDate", date)
      && SetDword(key, L"EstimatedSize", static_cast<DWORD>(std::min<std::uint64_t>(entry.mSizeBytes / 1024, 0xFFFF'FFFFu)))
      && SetDword(key, L"NoModify", 1) && SetDword(key, L"NoRepair", 1);
    RegCloseKey(key);
    return ok;
  } catch (...) {
    return false;
  }
}

bool RemoveUninstallEntry(const Layout& layout) noexcept {
  const LSTATUS result = RegDeleteKeyExW(HKEY_CURRENT_USER, layout.mUninstallKey.c_str(), KEY_WOW64_64KEY, 0);
  return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

bool GrantAppPackagesRead(const fs::path& folder) noexcept {
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(AppPackagesSid, &sid)) {
    return false;
  }
  PACL oldAcl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PACL newAcl = nullptr;
  bool ok = GetNamedSecurityInfoW(folder.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldAcl,
              nullptr, &descriptor)
    == ERROR_SUCCESS;
  if (ok) {
    EXPLICIT_ACCESSW access {};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    ok = SetEntriesInAclW(1, &access, oldAcl, &newAcl) == ERROR_SUCCESS
      && SetNamedSecurityInfoW(const_cast<LPWSTR>(folder.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
           nullptr, newAcl, nullptr)
        == ERROR_SUCCESS;
  }
  LocalFree(newAcl);
  LocalFree(descriptor);
  LocalFree(sid);
  return ok;
}

bool HasAppPackagesRead(const fs::path& folder) noexcept {
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(AppPackagesSid, &sid)) {
    return false;
  }
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  bool found = false;
  if (GetNamedSecurityInfoW(folder.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr,
        &descriptor)
        == ERROR_SUCCESS
      && acl) {
    for (DWORD i = 0; i < acl->AceCount && !found; ++i) {
      void* ace = nullptr;
      if (GetAce(acl, i, &ace) && static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE) {
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        const bool reads = (allowed->Mask & (GENERIC_READ | FILE_READ_DATA)) != 0;
        found = reads && EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), sid);
      }
    }
  }
  LocalFree(descriptor);
  LocalFree(sid);
  return found;
}

std::vector<FileUser> FindFileUsers(std::span<const fs::path> files) noexcept {
  std::vector<FileUser> users;
  try {
    RmSession session;
    if (session.Register(files)) {
      for (const auto& info: session.List()) {
        users.push_back({info.Process.dwProcessId, info.strAppName});
      }
    }
  } catch (...) {
  }
  return users;
}

CloseResult CloseHost(const fs::path& hostExe, std::chrono::milliseconds wait) noexcept {
  try {
    RmSession session;
    const fs::path files[] {hostExe};
    if (!session.Register(files)) {
      return CloseResult::NotRunning;
    }
    const auto running = session.List();
    if (running.empty()) {
      return CloseResult::NotRunning;
    }
    std::vector<HANDLE> processes;
    for (const auto& info: running) {
      if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, info.Process.dwProcessId)) {
        processes.push_back(process);
      }
    }
    // Not forced: the host is asked, and saves its settings first. A host
    // still starting up may not answer yet, so it's asked again every few
    // seconds until the deadline.
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + wait;
    bool allGone = false;
    while (!allGone) {
      session.Shutdown();
      const auto now = Clock::now();
      const auto round = std::min(deadline, now + std::chrono::seconds(3));
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(std::max(round - now, Clock::duration::zero()));
      if (processes.empty()) {
        // They couldn't be opened to wait on: asked, then listed again
        std::this_thread::sleep_for(left);
        allGone = session.List().empty();
      } else {
        const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(processes.size()), processes.data(), TRUE,
          static_cast<DWORD>(left.count()));
        allGone = result < WAIT_OBJECT_0 + processes.size();
      }
      if (Clock::now() >= deadline) {
        break;
      }
    }
    for (const HANDLE process: processes) {
      CloseHandle(process);
    }
    return allGone ? CloseResult::Closed : CloseResult::StillRunning;
  } catch (...) {
    return CloseResult::StillRunning;
  }
}

std::uint64_t FolderSize(const fs::path& folder) noexcept {
  std::uint64_t total = 0;
  try {
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
      if (it->is_regular_file(error)) {
        total += it->file_size(error);
      }
    }
  } catch (...) {
  }
  return total;
}

}// namespace OpenDesktopVR::Install
