// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Installing for one Windows user, with no administrator rights (M7a,
// ADR-043, ADR-009). The steps the setup program takes, each on its own, so
// the tests can check them in a private folder and registry key:
//
// - **Files:** each is written beside its target, then swapped in. A file
//   in use (a game holding the layer, an app holding the helper) is renamed
//   aside first: Windows allows renaming a loaded DLL, not replacing it.
//   Set-aside files are deleted once free, by the next setup and by each
//   host start (CMP-5: the game keeps the old layer until it restarts).
// - **What this install wrote** is listed in install-manifest.json, so the
//   next one removes files it no longer has, and an uninstall removes only
//   ours. The list is checked like anything from outside (TH-9).
// - **The Start menu shortcut, the Apps and Features entry** (under HKCU),
//   and read access for Store apps' sandbox (CMP-4, ADR-009's risk).
// - **The Restart Manager:** which processes use our files, and closing
//   the running host the way Windows closes programs when it signs out.
#pragma once

#include <OpenDesktopVR/Payload.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Install {

inline constexpr std::wstring_view SetupFileName = L"OpenDesktopVR-Setup.exe";
inline constexpr std::wstring_view HostFileName = L"OpenDesktopVR.exe";
inline constexpr std::wstring_view ManifestFileName = L"install-manifest.json";
// A file written beside its target, before it's swapped in
inline constexpr std::wstring_view NewSuffix = L".odvr-new";
// A file in use, renamed aside: "<name>.odvr-old-<n>"
inline constexpr std::wstring_view AsideSuffix = L".odvr-old";

struct Layout {
  std::filesystem::path mInstallFolder;
  std::filesystem::path mShortcut;// the Start menu's
  std::wstring mUninstallKey;// under HKEY_CURRENT_USER
  std::wstring mLayerKey;// where the layer is registered, under HKEY_CURRENT_USER
  std::filesystem::path mDataFolder;
  bool mTest {false};
};

// The real places (ADR-009). In a test instance (OPENDESKTOPVR_INSTANCE set)
// with OPENDESKTOPVR_INSTALL_ROOT, all inside that folder and under a
// private registry key, so a test never touches the user's install.
Layout DefaultLayout();

struct Installed {
  std::string mVersion;// empty: a test build installed by install.cmd
  std::filesystem::path mFolder;
};
// From the Apps and Features entry; or a test build's folder (no entry)
std::optional<Installed> FindInstalled(const Layout& layout) noexcept;

// How an update came about, so the new host can say so (ADR-045; round 25:
// a silent update gave no sign of itself)
enum class UpdatedBy {
  Setup,// the user ran the setup program: its own window said so
  Itself,// installed by itself (UP-4): the new host shows a notification
  Request,// "Install now" in the settings window: the new host reopens it
};
// The arguments, after the program's name, that start the host once
// `previous` is replaced: empty unless `previous` is a version
std::wstring HostArguments(std::string_view previous, UpdatedBy by);
// The host's side: the value after --updated-by; anything else is Setup
UpdatedBy ParseUpdatedBy(std::wstring_view text) noexcept;

using Progress = std::function<void(std::size_t done, std::size_t total)>;

struct WriteReport {
  bool mOk {false};
  std::size_t mWritten {0};
  std::size_t mSetAside {0};// in use: renamed aside, the new one in place
  std::size_t mRemoved {0};// the last install's, not in this one
  std::string mError;
};
// Writes the payload's files into `folder`, removes what the last install
// had that this one doesn't, and lists them all in the manifest, with
// `extra` (files the caller places itself: the setup program's own copy).
// With no manifest (a test build from before M7), its helper scripts and
// test tools are removed.
WriteReport WriteFiles(const Payload& payload, const std::filesystem::path& folder,
  std::span<const std::string> extra = {}, const Progress& progress = {}) noexcept;

// Deletes set-aside and half-written files in `folder` and its subfolders
// (one level down) that are free now; returns how many are still in use
std::size_t CleanAside(const std::filesystem::path& folder) noexcept;

struct RemoveReport {
  bool mOk {false};// the folder is gone
  std::size_t mRemoved {0};
  std::size_t mInUse {0};// set aside, still loaded somewhere
};
// The manifest's files (or, without one, the files this program ships),
// set-aside files, the manifest, then the folders, if empty
RemoveReport RemoveFiles(const std::filesystem::path& folder) noexcept;

// Copies `source` into `folder` (the setup program's own copy), setting the
// old one aside if it's running
bool PlaceFile(const std::filesystem::path& source, const std::filesystem::path& target) noexcept;

bool WriteShortcut(
  const std::filesystem::path& shortcut, const std::filesystem::path& target, std::wstring_view description) noexcept;
bool RemoveShortcut(const std::filesystem::path& shortcut) noexcept;

struct UninstallEntry {
  std::string mVersion;
  std::filesystem::path mFolder;
  std::filesystem::path mSetup;// the copy in the folder, which uninstalls
  std::uint64_t mSizeBytes {0};
};
bool WriteUninstallEntry(const Layout& layout, const UninstallEntry& entry) noexcept;
bool RemoveUninstallEntry(const Layout& layout) noexcept;

// "ALL APPLICATION PACKAGES" may read and run the folder's files, so a
// Store (packaged) OpenXR app can load the layer from a user folder (CMP-4)
bool GrantAppPackagesRead(const std::filesystem::path& folder) noexcept;
bool HasAppPackagesRead(const std::filesystem::path& folder) noexcept;

struct FileUser {
  unsigned long mProcessID {0};
  std::wstring mName;// the Restart Manager's name for it
};
// The processes using any of `files` (the Restart Manager)
std::vector<FileUser> FindFileUsers(std::span<const std::filesystem::path> files) noexcept;

enum class CloseResult { NotRunning, Closed, StillRunning };
// Asks the host running from `hostExe` to close, through the Restart
// Manager: Windows' end-of-session messages, which the host answers by
// saving and quitting. Waits up to `wait`.
CloseResult CloseHost(const std::filesystem::path& hostExe, std::chrono::milliseconds wait) noexcept;

std::uint64_t FolderSize(const std::filesystem::path& folder) noexcept;

}// namespace OpenDesktopVR::Install
