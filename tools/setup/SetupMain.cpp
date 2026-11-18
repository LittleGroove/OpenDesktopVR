// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// OpenDesktopVR-Setup.exe (M7a, ADR-043): installs, updates, reinstalls and
// uninstalls Open Desktop VR for the current Windows user, with no
// administrator rights (ADR-009). The program's files ride inside it as a
// resource, which odvr-pack adds after it's built.
//
//   OpenDesktopVR-Setup.exe               install, update or reinstall; asks first
//   OpenDesktopVR-Setup.exe --uninstall   asks first (Apps and Features runs this)
//   --silent        no questions and no windows (the app's updater, M7b; tests)
//   --remove-data   with --uninstall --silent: settings, profiles and logs too
//   --no-start      leaves Open Desktop VR closed afterwards
//   --allow-older   with --silent: replaces a newer version
//
// Exit codes: 0 done; 1 failed; 2 cancelled; 3 Open Desktop VR wouldn't
// close; 4 refused (as administrator, older than what's installed, or
// another setup running). Its log: <data folder>\logs\setup.log.

#include <OpenDesktopVR/Installer.hpp>
#include <OpenDesktopVR/LayerRegistration.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Payload.hpp>
#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/Version.hpp>

#include <Windows.h>

#include <CommCtrl.h>
#include <shellapi.h>

#include <atomic>
#include <format>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Task dialogs come with version 6 of the common controls
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using namespace OpenDesktopVR;
using namespace std::chrono_literals;

namespace {

enum ExitCode : int { Done = 0, Failed = 1, Cancelled = 2, HostStillRunning = 3, Refused = 4 };

constexpr wchar_t Title[] = L"Open Desktop VR setup";
// Dialog units: wide enough for a folder path on one line
constexpr UINT DialogWidth = 300;
constexpr wchar_t ShortcutDescription[] = L"Your desktop's windows in VR, used with your hands";

struct Options {
  bool mUninstall {false};
  bool mSilent {false};
  bool mRemoveData {false};
  bool mNoStart {false};
  bool mAllowOlder {false};
  bool mFromCopy {false};// the uninstaller's copy in the temp folder
};

Options ParseOptions() {
  Options options;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring_view arg {argv[i]};
    options.mUninstall = options.mUninstall || arg == L"--uninstall";
    options.mSilent = options.mSilent || arg == L"--silent";
    options.mRemoveData = options.mRemoveData || arg == L"--remove-data";
    options.mNoStart = options.mNoStart || arg == L"--no-start";
    options.mAllowOlder = options.mAllowOlder || arg == L"--allow-older";
    options.mFromCopy = options.mFromCopy || arg == L"--from-copy";
  }
  LocalFree(argv);
  return options;
}

std::wstring UserName() {
  wchar_t name[256] {};
  DWORD size = static_cast<DWORD>(std::size(name));
  return GetUserNameW(name, &size) ? std::wstring {name} : std::wstring {L"this Windows user"};
}

std::wstring LogPath() {
  return (Product::LogsFolder() / L"setup.log").wstring();
}

// ---- Dialogs: Windows' own task dialogs, in the user's theme -------------

struct Choice {
  int mID;
  const wchar_t* mText;
};

int Ask(PCWSTR icon, const std::wstring& heading, const std::wstring& text, std::initializer_list<Choice> choices,
  int defaultID, const wchar_t* checkText = nullptr, bool* checked = nullptr) {
  std::vector<TASKDIALOG_BUTTON> buttons;
  for (const auto& choice: choices) {
    buttons.push_back({choice.mID, choice.mText});
  }
  TASKDIALOGCONFIG config {sizeof(config)};
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.cxWidth = DialogWidth;
  config.pszWindowTitle = Title;
  config.pszMainIcon = icon;
  config.pszMainInstruction = heading.c_str();
  config.pszContent = text.c_str();
  config.pButtons = buttons.data();
  config.cButtons = static_cast<UINT>(buttons.size());
  config.nDefaultButton = defaultID;
  if (checkText && checked) {
    config.pszVerificationText = checkText;
    if (*checked) {
      config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    }
  }
  int pressed = IDCANCEL;
  BOOL verified = FALSE;
  if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, &verified))) {
    return IDCANCEL;
  }
  if (checked) {
    *checked = verified != FALSE;
  }
  return pressed;
}

void Tell(PCWSTR icon, const std::wstring& heading, const std::wstring& text) {
  Ask(icon, heading, text, {{IDOK, L"Close"}}, IDOK);
}

// What a run of steps ended with: the exit code, and what to tell the user
struct Outcome {
  int mCode {Failed};
  std::wstring mHeading;
  std::wstring mText;
};

// The steps' progress, shared with the dialog's timer on the UI thread
struct Work {
  std::mutex mMutex;
  std::wstring mText;
  std::atomic<int> mPercent {0};
  std::atomic<bool> mDone {false};

  void Say(const std::wstring& text, int percent) {
    Log::Info("Setup: {}", StringUtil::Narrow(text));
    {
      std::lock_guard lock(mMutex);
      mText = text;
    }
    mPercent = percent;
  }
};

struct ProgressState {
  Work* mWork {nullptr};
  std::wstring mShown;
};

HRESULT CALLBACK ProgressCallback(HWND window, UINT notification, WPARAM, LPARAM, LONG_PTR data) {
  auto& state = *reinterpret_cast<ProgressState*>(data);
  switch (notification) {
    case TDN_CREATED:
      SendMessageW(window, TDM_ENABLE_BUTTON, IDCLOSE, FALSE);
      return S_OK;
    case TDN_TIMER: {
      std::wstring text;
      {
        std::lock_guard lock(state.mWork->mMutex);
        text = state.mWork->mText;
      }
      if (text != state.mShown && !text.empty()) {
        state.mShown = text;
        SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(state.mShown.c_str()));
      }
      SendMessageW(window, TDM_SET_PROGRESS_BAR_POS, state.mWork->mPercent.load(), 0);
      if (state.mWork->mDone) {
        SendMessageW(window, TDM_ENABLE_BUTTON, IDCLOSE, TRUE);
        SendMessageW(window, TDM_CLICK_BUTTON, IDCLOSE, 0);
      }
      return S_OK;
    }
    case TDN_BUTTON_CLICKED:
      return state.mWork->mDone ? S_OK : S_FALSE;// stays open until the steps finish
  }
  return S_OK;
}

// Runs `steps` on a worker while a progress dialog shows (silent: no dialog)
template <class Steps>
Outcome RunSteps(bool silent, const std::wstring& heading, Steps&& steps) {
  Work work;
  Outcome outcome {Failed, L"Setup stopped unexpectedly", L"The log may say why: " + LogPath()};
  std::thread worker([&] {
    ThreadUtil::SetCurrentThreadName(L"setup steps");
    ThreadUtil::CatchAll("setup steps", [&] { outcome = steps(work); });
    work.mDone = true;
  });
  if (!silent) {
    ProgressState state {&work};
    TASKDIALOGCONFIG config {sizeof(config)};
    config.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER;
    config.cxWidth = DialogWidth;
    config.pszWindowTitle = Title;
    config.pszMainInstruction = heading.c_str();
    config.pszContent = L"Starting...";
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pfCallback = ProgressCallback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&state);
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
  }
  worker.join();
  return outcome;
}

Registration::RegistryLocation LayerLocation(const Install::Layout& layout) {
  Registration::RegistryLocation location;
  location.mKeyPath = layout.mLayerKey;
  return location;
}

std::wstring Names(const std::vector<Install::FileUser>& users) {
  std::wstring names;
  for (const auto& user: users) {
    if (names.find(user.mName) == std::wstring::npos) {
      names += (names.empty() ? L"" : L", ") + user.mName;
    }
  }
  return names;
}

bool StartHost(const fs::path& host) {
  std::wstring line = L"\"" + host.wstring() + L"\"";
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(host.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, host.parent_path().c_str(),
        &startup, &info)) {
    Log::Warning("Setup: couldn't start {} (error {})", StringUtil::Narrow(host.wstring()), GetLastError());
    return false;
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return true;
}

Outcome StillRunning() {
  return {HostStillRunning, L"Open Desktop VR is still running",
    L"Exit it from its tray icon (right-click it, then choose Exit), then run setup again. Nothing was changed."};
}

// ---- Install, update, reinstall --------------------------------------------

Outcome InstallSteps(
  Work& work, const Install::Layout& layout, const Install::Payload& payload, const fs::path& self, const Options& options) {
  const auto& folder = layout.mInstallFolder;
  const auto host = folder / Install::HostFileName;
  work.Say(L"Closing Open Desktop VR...", 2);
  const auto closed = Install::CloseHost(host, 15s);
  if (closed == Install::CloseResult::StillRunning) {
    return StillRunning();
  }
  Log::Info("Setup: Open Desktop VR {}", closed == Install::CloseResult::Closed ? "was running: closed" : "wasn't running");
  // They keep the old ones until they restart (CMP-5)
  const fs::path layer[] {folder / Product::LayerDllName};
  const fs::path helper[] {folder / Product::AppHelperDllName};
  const auto games = Install::FindFileUsers(layer);
  const auto apps = Install::FindFileUsers(helper);

  work.Say(L"Copying its files...", 5);
  const std::string extra[] {StringUtil::Narrow(Install::SetupFileName)};
  const auto report = Install::WriteFiles(payload, folder, extra, [&work](std::size_t done, std::size_t total) {
    work.mPercent = 5 + static_cast<int>(80 * done / std::max<std::size_t>(total, 1));
  });
  if (!report.mOk) {
    Log::Error("Setup: the files failed: {}", report.mError);
    return {Failed, L"Open Desktop VR couldn't be installed",
      StringUtil::Widen(report.mError) + L".\n\nClose any game and app using it, then run setup again. The log: "
        + LogPath()};
  }
  Log::Info("Setup: {} files written, {} in use set aside, {} old ones removed", report.mWritten, report.mSetAside,
    report.mRemoved);
  if (!Install::PlaceFile(self, folder / Install::SetupFileName)) {
    return {Failed, L"Open Desktop VR couldn't be installed", L"Setup couldn't copy itself into " + folder.wstring()};
  }

  work.Say(L"Adding it to the Start menu and to Apps and Features...", 88);
  if (!Install::GrantAppPackagesRead(folder)) {
    Log::Warning("Setup: couldn't let Store apps read the folder (CMP-4)");
  }
  if (!Install::WriteShortcut(layout.mShortcut, host, ShortcutDescription)) {
    Log::Warning("Setup: couldn't make the Start menu shortcut");
  }
  const Install::UninstallEntry entry {payload.mVersion, folder, folder / Install::SetupFileName, Install::FolderSize(folder)};
  if (!Install::WriteUninstallEntry(layout, entry)) {
    return {Failed, L"Open Desktop VR couldn't be installed", L"Setup couldn't add it to Apps and Features."};
  }

  work.Say(L"Registering its in-game part...", 94);
  const auto registered = Registration::Register(folder / Product::LayerManifestName, LayerLocation(layout));
  if (!registered.mOk) {
    return {Failed, L"Open Desktop VR couldn't be installed",
      std::format(L"Setup couldn't register its in-game part (error {}).", registered.mError)};
  }
  Log::Info("Setup: the layer is registered{}", registered.mStaleDisabled ? " (older entries of ours disabled)" : "");
  work.Say(L"Done.", 100);

  Outcome outcome {Done, std::format(L"Open Desktop VR {} is installed", StringUtil::Widen(payload.mVersion)), L""};
  if (!options.mNoStart && StartHost(host)) {
    outcome.mText = L"It's starting now. Its settings window opens the first time; after that, it waits in the tray "
                    L"by the clock, and starting it again from the Start menu opens its window.";
  } else {
    outcome.mText = L"Start it from the Start menu: Open Desktop VR.";
  }
  if (!games.empty()) {
    outcome.mText += L"\n\nStill using the old in-game part until restarted: " + Names(games) + L".";
  }
  if (!apps.empty()) {
    outcome.mText += L"\n\nStill using the old helper until restarted: " + Names(apps)
      + L". If taps stop reaching their panels, restart them.";
  }
  if (!games.empty() || !apps.empty()) {
    Log::Info("Setup: still on the old files until restarted: games '{}', apps '{}'", StringUtil::Narrow(Names(games)),
      StringUtil::Narrow(Names(apps)));
  }
  return outcome;
}

int Uninstall(const Options& options, const fs::path& self);

int InstallOrUpdate(const Options& options, const Install::Payload& payload, const fs::path& self) {
  const auto layout = Install::DefaultLayout();
  const auto version = Install::ParseVersion(payload.mVersion).value_or(Install::Version {});
  const auto installed = Install::FindInstalled(layout);
  const auto newText = StringUtil::Widen(payload.mVersion);
  const auto folderText = layout.mInstallFolder.wstring();
  Log::Info("Setup: version {} ({}); installed: {}", payload.mVersion, payload.mBuild,
    installed ? (installed->mVersion.empty() ? std::string {"a test build"} : installed->mVersion) : std::string {"none"});

  std::wstring heading;
  std::wstring text;
  std::wstring verb = L"Install";
  bool offerUninstall = false;
  bool older = false;
  if (!installed) {
    heading = L"Install Open Desktop VR " + newText + L"?";
    text = L"For " + UserName() + L" only, in " + folderText
      + L". No administrator rights needed.\n\nIt adds itself to the Start menu, and to Apps and Features, which "
        L"can uninstall it.";
  } else {
    const auto old = Install::ParseVersion(installed->mVersion);
    if (!old || *old < version) {
      heading = installed->mVersion.empty() ? L"Update the test build of Open Desktop VR to " + newText + L"?"
                                            : L"Update Open Desktop VR " + StringUtil::Widen(installed->mVersion)
          + L" to " + newText + L"?";
      text = L"Your panels, profiles and settings stay as they are. If it's running, it closes and starts again.";
      verb = L"Update";
    } else if (*old == version) {
      heading = L"Open Desktop VR " + newText + L" is already installed";
      text = L"Reinstall it to repair it (your panels, profiles and settings stay as they are), or uninstall it.";
      verb = L"Reinstall";
      offerUninstall = true;
    } else {
      older = true;
      heading = L"A newer version, " + StringUtil::Widen(installed->mVersion) + L", is installed";
      text = L"Replace it with " + newText + L"? Your panels, profiles and settings stay as they are.";
      verb = L"Replace";
    }
  }
  if (options.mSilent) {
    if (older && !options.mAllowOlder) {
      Log::Warning("Setup: refused: a newer version is installed (--allow-older replaces it)");
      return Refused;
    }
  } else {
    const int choice = offerUninstall
      ? Ask(nullptr, heading, text, {{IDOK, verb.c_str()}, {IDRETRY, L"Uninstall"}, {IDCANCEL, L"Cancel"}}, IDOK)
      : Ask(older ? TD_WARNING_ICON : nullptr, heading, text, {{IDOK, verb.c_str()}, {IDCANCEL, L"Cancel"}},
          older ? IDCANCEL : IDOK);
    if (choice == IDRETRY) {
      Options uninstall = options;
      uninstall.mUninstall = true;
      return Uninstall(uninstall, self);
    }
    if (choice != IDOK) {
      Log::Info("Setup: cancelled");
      return Cancelled;
    }
  }
  const auto outcome = RunSteps(options.mSilent, L"Installing Open Desktop VR " + newText,
    [&](Work& work) { return InstallSteps(work, layout, payload, self, options); });
  if (!options.mSilent) {
    Tell(outcome.mCode == Done ? TD_INFORMATION_ICON : TD_ERROR_ICON, outcome.mHeading, outcome.mText);
  }
  return outcome.mCode;
}

// ---- Uninstall --------------------------------------------------------------

bool IsInside(const fs::path& file, const fs::path& folder) {
  std::error_code error;
  return fs::equivalent(file.parent_path(), folder, error);
}

// Old copies of the uninstaller in the temp folder (it can't delete itself)
void CleanOldCopies(const fs::path& self) {
  std::error_code error;
  const auto temp = fs::temp_directory_path(error);
  for (const auto& entry: fs::directory_iterator(temp, fs::directory_options::skip_permission_denied, error)) {
    const auto name = entry.path().filename().native();
    if (name.starts_with(L"OpenDesktopVR-Uninstall-") && name.ends_with(L".exe") && !fs::equivalent(entry.path(), self, error)) {
      DeleteFileW(entry.path().c_str());
    }
  }
}

int Uninstall(const Options& options, const fs::path& self) {
  const auto layout = Install::DefaultLayout();
  const auto& folder = layout.mInstallFolder;
  // Apps and Features runs the copy in the folder it removes: a copy in the
  // temp folder does it instead, and this one ends so its file can go
  if (!options.mFromCopy && IsInside(self, folder)) {
    std::error_code error;
    const auto copy = fs::temp_directory_path(error) / std::format(L"OpenDesktopVR-Uninstall-{}.exe", GetCurrentProcessId());
    std::wstring line = L"\"" + copy.wstring() + L"\" --uninstall --from-copy";
    line += options.mSilent ? L" --silent" : L"";
    line += options.mRemoveData ? L" --remove-data" : L"";
    STARTUPINFOW startup {sizeof(startup)};
    PROCESS_INFORMATION info {};
    if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)
        || !CreateProcessW(copy.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, copy.parent_path().c_str(),
          &startup, &info)) {
      Log::Error("Setup: couldn't run the uninstaller from the temp folder (error {})", GetLastError());
      if (!options.mSilent) {
        Tell(TD_ERROR_ICON, L"Open Desktop VR couldn't be uninstalled", L"Setup couldn't copy itself to the temp folder.");
      }
      return Failed;
    }
    Log::Info("Setup: uninstalling from a copy in the temp folder (process {})", info.dwProcessId);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return Done;
  }

  bool removeData = options.mRemoveData;
  if (!options.mSilent) {
    const int choice = Ask(nullptr, L"Uninstall Open Desktop VR?",
      L"It's removed for " + UserName()
        + L": the program, its Start menu entry and its in-game part. Games stop showing its panels.",
      {{IDOK, L"Uninstall"}, {IDCANCEL, L"Cancel"}}, IDOK, L"Also delete my panels, profiles, settings and logs",
      &removeData);
    if (choice != IDOK) {
      Log::Info("Setup: uninstall cancelled");
      return Cancelled;
    }
  }
  const auto outcome = RunSteps(options.mSilent, L"Uninstalling Open Desktop VR", [&](Work& work) -> Outcome {
    work.Say(L"Closing Open Desktop VR...", 5);
    if (Install::CloseHost(folder / Install::HostFileName, 15s) == Install::CloseResult::StillRunning) {
      return StillRunning();
    }
    work.Say(L"Removing its in-game part...", 20);
    const int unregistered = Registration::Unregister(LayerLocation(layout));
    Log::Info("Setup: {} layer entries removed", unregistered);
    work.Say(L"Removing its files...", 40);
    auto removed = Install::RemoveFiles(folder);
    // Apps let go of the helper shortly after the host has gone
    for (int retry = 0; !removed.mOk && removed.mInUse > 0 && retry < 10; ++retry) {
      std::this_thread::sleep_for(500ms);
      const auto again = Install::RemoveFiles(folder);
      removed.mOk = again.mOk;
      removed.mInUse = again.mInUse;
    }
    Log::Info("Setup: {} files removed, {} still in use; the folder {}", removed.mRemoved, removed.mInUse,
      removed.mOk ? "is gone" : "is still there");
    std::wstring holders;
    if (!removed.mOk) {
      std::vector<fs::path> left;
      std::error_code error;
      for (auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator();
           it.increment(error)) {
        if (it->is_regular_file(error)) {
          left.push_back(it->path());
        }
      }
      holders = Names(Install::FindFileUsers(left));
      Log::Info("Setup: still holding files: '{}'", StringUtil::Narrow(holders));
    }
    Install::RemoveShortcut(layout.mShortcut);
    Install::RemoveUninstallEntry(layout);
    Outcome done {Done, L"Open Desktop VR is uninstalled", L""};
    if (removeData) {
      work.Say(L"Deleting its settings and logs...", 90);
      Log::Info("Setup: deleting {}", StringUtil::Narrow(layout.mDataFolder.wstring()));
      Log::Stop();
      std::error_code error;
      fs::remove_all(layout.mDataFolder, error);
      done.mText = L"Its panels, profiles, settings and logs are deleted too.";
    } else {
      done.mText = L"Your panels, profiles and settings are kept, in " + layout.mDataFolder.wstring()
        + L", for a later install. Delete that folder to remove them.";
    }
    if (!removed.mOk) {
      done.mText += L"\n\nSome of its files are still in use"
        + (holders.empty() ? std::wstring {L" by a game or an app"} : L" by " + holders)
        + L". Close it, then delete " + folder.wstring() + L".";
    }
    work.Say(L"Done.", 100);
    return done;
  });
  if (!options.mSilent) {
    Tell(outcome.mCode == Done ? TD_INFORMATION_ICON : TD_ERROR_ICON, outcome.mHeading, outcome.mText);
  }
  return outcome.mCode;
}

// ---- Start ------------------------------------------------------------------

std::optional<Install::Payload> LoadPayload(std::string& error) {
  const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(Install::PayloadResourceID), RT_RCDATA);
  const HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
  const void* data = loaded ? LockResource(loaded) : nullptr;
  if (!data) {
    error = "it carries no files";
    return std::nullopt;
  }
  return Install::Unpack({static_cast<const char*>(data), SizeofResource(nullptr, resource)}, &error);
}

int Run(const Options& options) {
  const auto instance = ProcessUtil::GetEnvironment("OPENDESKTOPVR_INSTANCE").value_or("");
  const auto mutexName = L"Local\\OpenDesktopVR.Setup" + StringUtil::Widen(instance);
  const HANDLE single = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
    Log::Warning("Setup: another setup is running");
    if (!options.mSilent) {
      Tell(TD_WARNING_ICON, L"Setup is already running", L"Finish the other one first.");
    }
    return Refused;
  }
  // Per user: as administrator, it would install for the administrator's
  // account, and start Open Desktop VR elevated, where games can't see its
  // panels (CMP-8)
  if (ProcessUtil::CurrentProcessElevation() == ProcessUtil::Elevation::Elevated && ProcessUtil::UacEnabled().value_or(true)) {
    Log::Warning("Setup: refused: running as administrator");
    if (!options.mSilent) {
      Tell(TD_WARNING_ICON, L"Run setup normally, not as administrator",
        L"Open Desktop VR installs for your Windows user only, and needs no administrator rights. Started as "
        L"administrator, games couldn't show its panels.");
    }
    return Refused;
  }
  const auto self = ProcessUtil::ExecutablePath();
  CleanOldCopies(self);
  if (options.mUninstall) {
    return Uninstall(options, self);
  }
  std::string error;
  const auto payload = LoadPayload(error);
  if (!payload) {
    Log::Error("Setup: its payload is unusable: {}", error);
    if (!options.mSilent) {
      Tell(TD_ERROR_ICON, L"This setup program is damaged",
        L"Download it again. (" + StringUtil::Widen(error) + L")");
    }
    return Failed;
  }
  return InstallOrUpdate(options, *payload, self);
}

}// namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Per-monitor DPI comes from setup.manifest
  const auto options = ParseOptions();
  Log::Config config;
  if (const auto logs = Product::LogsFolder(); !logs.empty()) {
    config.mFile = logs / L"setup.log";
  }
  Log::Start(config);
  Log::Info("Open Desktop VR setup started ({}{}{})", options.mUninstall ? "uninstall" : "install",
    options.mSilent ? ", silent" : "", options.mFromCopy ? ", from its temp copy" : "");
  int code = Failed;
  ThreadUtil::CatchAll("setup", [&] { code = Run(options); });
  Log::Info("Setup finished: exit code {}", code);
  Log::Stop();
  return code;
}
