// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The desktop settings window (doc 09 "Desktop UI", ADR-020): the panel
// list, a window picker, and each panel's window rule, placement, crop and
// update rate. Dear ImGui on its own small D3D11 device, on the UI thread:
// it can't stall the panel renderer, and the renderer can't stall it.
//
// - Paints only in response to input, and for half a second after (hover
//   and animation), at most 30 times a second; nothing while idle (RN-12).
// - Opens within doc 08's 50 ms: its device and its fonts (at the main
//   monitor's scale) are made on a worker thread at start and kept; its
//   window, swapchain and ImGui context exist only while it's open, and its
//   first frame is drawn before it shows.
// - Window enumeration for the picker runs on a worker thread (ADR-020).
// - Edits are addressed by panel ID and dropped if the panel is gone (DU-2),
//   touch only that panel (ST-6), and save through the debounced store
//   (ST-1). Text fields apply when you leave them, so a window rule isn't
//   re-applied on every keystroke.
#pragma once

#include "KeybindService.hpp"
#include "PanelManager.hpp"
#include "Support.hpp"

#include <OpenDesktopVR/AppHelperService.hpp>
#include <OpenDesktopVR/HostLink.hpp>
#include <OpenDesktopVR/KeyboardLayout.hpp>
#include <OpenDesktopVR/LogRing.hpp>
#include <OpenDesktopVR/RenderLoop.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>
#include <OpenDesktopVR/UiTheme.hpp>
#include <OpenDesktopVR/WindowUtil.hpp>

#include <Windows.h>

#include <atomic>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <functional>
#include <span>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct ImFontAtlas;
struct ImGuiContext;

namespace OpenDesktopVR::Host {

class UpdateService;

class SettingsWindow {
 public:
  SettingsWindow(HINSTANCE instance, RenderLoop& renderer, PanelManager& panels, KeybindService& keybinds,
    AppHelper::AppHelperService& appHelper, const Log::LogRing& logRing);
  ~SettingsWindow();
  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  // Opens the window, or brings it to the front
  void Show();
  bool IsOpen() const noexcept {
    return mWindow != nullptr;
  }
  // The panels changed elsewhere (the tray menu): repaint if open
  void Invalidate();
  // The same from any thread (a document's page count arrived): it's
  // posted, never waited for
  void PostInvalidate() noexcept;

  // Tests: half a second after opening (and once the picker, if asked for,
  // has its list), save the window as a PNG; then stay idle for two
  // seconds, write how many times it painted in the second of those, how
  // many input messages it had then, and how long it took to open, to
  // "<png>.idle.txt" (no paints is a pass: RN-12), and quit the host
  void RequestScreenshot(std::filesystem::path path, bool withPicker);
  // Opens on the General tab (an update's notice or "Install now"; tests)
  void ShowGeneralTab() noexcept {
    mSelectGeneral = true;
  }
  void ShowProfilesTab() noexcept {
    mSelectProfiles = true;
  }
  // Tests: show every tab once, one a frame, and report the longest switch
  void CycleTabs() noexcept {
    mCycleTab = 0;
  }
  void ShowHandsTab() noexcept {
    mSelectHands = true;
  }
  void ShowKeybindsTab() noexcept {
    mSelectKeybinds = true;
  }
  // A first start (M6f): a welcome above the panel list until it's closed
  void ShowWelcome() noexcept {
    mWelcome = true;
  }
  // Tests: the Keybinds tab with its command builder open
  void ShowCommandBuilder() noexcept {
    mSelectKeybinds = true;
    mOpenCommandBuilder = true;
  }
  void ShowDiagnosticsTab() noexcept {
    mSelectDiagnostics = true;
  }
  // The Hands tab's "Open it now" for the quick menu (ADR-041)
  void SetQuickMenuOpener(std::function<void()> open) {
    mOpenQuickMenu = std::move(open);
  }
  // The General tab's Updates section (ADR-045); it outlives this window's use
  void SetUpdates(UpdateService* updates) noexcept {
    mUpdates = updates;
  }
  // The command builder's "Try it" (UI-9, ADR-042): runs a command's words
  // and says what happened
  void SetCommandRunner(std::function<std::string(std::span<const std::wstring>)> run) {
    mRunCommand = std::move(run);
  }

 private:
  struct Gpu {
    winrt::com_ptr<ID3D11Device> mDevice;
    winrt::com_ptr<ID3D11DeviceContext> mContext;
  };
  // The default adapter, or WARP; empty if neither works
  static Gpu CreateDevice() noexcept;
  // What the worker makes at start: the device, and the fonts for the main
  // monitor's scale
  struct Prepared {
    Gpu mGpu;
    std::shared_ptr<ImFontAtlas> mAtlas;
    UiTheme::Fonts mFonts;
    float mScale {0};
  };
  static Prepared Prepare() noexcept;

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
  bool Create();
  void Destroy();
  bool CreateTarget();
  void Resize();
  void Paint();
  void KeepPainting(int frames);
  void SetScale(float scale);
  void TakeScreenshot();
  void OnScreenshotTimer(UINT_PTR timer);

  // The interface, built each paint
  void BuildUi();
  void PanelList(const std::vector<PanelDefinition>& panels);
  void PanelDetails(PanelDefinition panel);

  // The Keybinds tab (doc 09 "Bind UI"): every action's two slots, and the
  // "press the keys you want" capture (KY-4)
  void KeybindsPage(const std::vector<PanelDefinition>& panels);
  void HandsPage();
  // Performance, games, startup and about
  void GeneralPage();
  // Diagnostics and support (M6c, doc 09): the "doesn't show in game"
  // self-test (UI-10), the game's status, each app's helper, A/B switches,
  // the log viewer (LG-3), and "Save debug info" (LG-4)
  void DiagnosticsPage();
  void SelfTestSection();
  void LogSection();
  void SupportSection();
  void PrivacyPopup();
  // The self-test runs on a worker (it reads the registry and other
  // processes); its result shows when ready
  void StartSelfTest();
  void TakeSelfTest();
  void StartBundle(std::filesystem::path zip);
  // Once a second while the tab shows: repaint only if what it shows changed
  void CheckDiagnostics();
  // Profiles (ADR-038): switch, make, rename, delete, games, export, import
  void ProfilesPage();
  // A native file dialog for a profile file, or a debug-info .zip; nullopt
  // if cancelled. Only ever outside a frame: its modal loop paints this
  // window (round 15)
  std::optional<std::filesystem::path> AskProfileFile(bool save, const std::wstring& name, bool debugInfo = false);
  // A PDF or an image, or a folder (M8a); panelID 0 for a new panel
  std::optional<std::filesystem::path> AskDocument(bool folder);
  void AskForDocument(bool folder, uint64_t panelID);
  void DocumentSection(PanelDefinition& panel, bool& changed);
  // The export or import asked for during a frame, run once it's done
  void RunPendingDialog();
  // The virtual keyboard's settings, on the Hands tab (K10)
  void KeyboardSettingsUi(bool handsEnabled);
  // The quick menu's, below them (ADR-041)
  void QuickMenuSettingsUi(bool handsEnabled);
  bool LayoutCombo(const char* label, std::string* klid, const char* none);
  void SlotButton(const Keybinds::Action& action, std::size_t slot, const Keybinds::Binding& binding);
  void StartCapture(const Keybinds::Action& action, std::size_t slot);
  void CapturePopup();
  void Assign(const Keybinds::Action& action, std::size_t slot, const Keybinds::Binding& binding);
  std::optional<Keybinds::Slots> SlotsFor(const Keybinds::Action& action) const;
  void WindowPicker();
  void StartListing();
  void OpenPicker(bool retarget);
  void Pick(const WindowUtil::WindowInfo& window);

  // Text fields: loaded when a panel is selected, applied when you leave
  // them, or to the panel they came from if you select another first
  void LoadTextFields(const PanelDefinition& panel);
  void ApplyTextFields(PanelDefinition& panel) const;
  void CommitTextFields();

  HINSTANCE mInstance {nullptr};
  RenderLoop& mRenderer;
  PanelManager& mPanels;
  KeybindService& mKeybinds;
  AppHelper::AppHelperService& mAppHelper;
  const Log::LogRing& mLogRing;
  Events::Subscription mPanelsChanged;// repaints when keybinds hide panels

  // A keybind being set
  std::shared_ptr<Keybinds::CaptureSession> mCapture;
  Keybinds::Action mCaptureAction {};
  std::size_t mCaptureSlot {0};
  TimeUtil::TimePoint mCaptureStarted {};
  std::optional<TimeUtil::TimePoint> mCaptureDoneAt;
  std::optional<Keybinds::Binding> mConflict;// the binding, while asking
  Keybinds::Action mConflictWith {};
  bool mOpenCapture {false};
  bool mSelectKeybinds {false};
  bool mSelectHands {false};

  HWND mWindow {nullptr};
  std::atomic<HWND> mPostTarget {nullptr};// mWindow, for other threads
  // Made in the background at start and kept while the host runs (an idle
  // device costs no CPU); the rest exists only while the window is open
  ThreadUtil::BackgroundTask<Prepared> mPrepareTask;
  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext> mContext;
  winrt::com_ptr<IDXGISwapChain1> mSwapchain;
  winrt::com_ptr<ID3D11RenderTargetView> mTarget;
  ImGuiContext* mImGui {nullptr};
  bool mPlatformReady {false};// ImGui's Win32 backend
  bool mRendererReady {false};// ImGui's D3D11 backend
  int mDeviceLosses {0};
  UiTheme::Fonts mFonts;
  // The fonts' atlas, shared with the ImGui context and kept between opens;
  // built for mAtlasScale (0: not built yet)
  std::shared_ptr<ImFontAtlas> mAtlas;
  float mAtlasScale {0};
  float mScale {1.0f};
  bool mRebuildFonts {false};
  bool mOccluded {false};
  int mFramesLeft {0};
  bool mTimerRunning {false};
  uint64_t mPaints {0};

  // What's selected, and the text fields being edited for it
  uint64_t mSelected {0};
  // The Profiles tab: the profile selected, its name being edited, and a
  // game being typed
  std::string mSelectedProfile;
  std::string mProfileNameFor;
  std::array<char, 128> mProfileName {};
  std::array<char, 128> mNewGame {};
  bool mSelectProfiles {false};
  bool mSelectGeneral {false};
  std::array<char, 128> mNewDisabledGame {};
  std::optional<bool> mStartWithWindows;// read once the General tab shows
  // DU-3 (tests: --settings-cycle-tabs): every tab shown once, one a
  // frame, timing each switch
  int mCycleTab {-1};
  double mLongestSwitchMs {0};
  struct PendingDialog {
    bool mSave {false};// export; otherwise import
    std::string mProfileID;
    std::wstring mName;
    bool mDebugInfo {false};// "Save debug info": a .zip, then the bundle
    // A document panel's file or folder (M8a): a new panel, or for this one
    bool mDocument {false};
    bool mFolder {false};
    uint64_t mForPanel {0};
  };
  std::optional<PendingDialog> mPendingDialog;
  // Inside a frame: a frame never starts inside another (round 15: a file
  // dialog opened mid-frame painted this window from its modal loop)
  bool mInFrame {false};
  uint64_t mFieldsFor {0};
  std::array<char, 256> mName {};
  std::array<char, 1024> mExecutable {};
  std::array<char, 512> mClass {};
  std::array<char, 512> mTitle {};
  bool mFieldsEdited {false};

  // The window picker
  bool mOpenPicker {false};
  bool mPickerRetargets {false};// "Use a different window" for the selected panel
  ThreadUtil::BackgroundTask<std::vector<WindowUtil::WindowInfo>> mListing;
  // The listed windows' pictures (doc 09): taken on a worker after the list
  // arrives, then textures on this window's device
  using Thumbnails = std::vector<std::pair<HWND, WindowUtil::Thumbnail>>;
  ThreadUtil::BackgroundTask<Thumbnails> mThumbnailTask;
  std::map<HWND, winrt::com_ptr<ID3D11ShaderResourceView>> mThumbnails;
  void StartThumbnails();
  void TakeThumbnails();
  // The selected panel as the layer gets it (doc 09 "a big live preview"):
  // the shared textures, opened on this window's device, like the mirror.
  // Redrawn only when its content changes, at most 10 times a second.
  void PanelPreview(const PanelDefinition& panel);
  void CheckPreview();
  std::unique_ptr<HostLink::Client> mLink;
  std::map<ID3D11Texture2D*, winrt::com_ptr<ID3D11ShaderResourceView>> mPreviewViews;
  uint64_t mPreviewPanel {0};
  uint64_t mPreviewVersion {0};
  bool mPreviewDrawn {false};// this frame
  bool mPreviewShowing {false};// an image, not a placeholder
  // The installed keyboard layouts, read in the background the first time
  // the Hands tab shows (reading 200 of them takes a moment)
  ThreadUtil::BackgroundTask<std::vector<KeyboardLayout::LayoutInfo>> mLayoutsTask;
  std::optional<std::vector<KeyboardLayout::LayoutInfo>> mLayouts;
  std::vector<WindowUtil::WindowInfo> mWindows;
  std::array<char, 128> mFilter {};
  int mPickerSelection {-1};

  // The Diagnostics tab
  bool mSelectDiagnostics {false};
  std::function<void()> mOpenQuickMenu;
  UpdateService* mUpdates {nullptr};
  void UpdatesSection();
  // The Keybinds tab's command builder (UI-9)
  void CommandBuilder(const std::vector<PanelDefinition>& panels);
  std::function<std::string(std::span<const std::wstring>)> mRunCommand;
  int mCommandKind {0};
  int mCommandTarget {0};
  int mCommandDirection {0};
  float mCommandAmount {1};
  std::array<char, 16> mCommandLevel {"+10"};
  std::array<char, 128> mCommandTitle {};
  std::string mCommandResult;
  bool mOpenCommandBuilder {false};
  bool mWelcome {false};
  bool mDiagnosticsDrawn {false};// this frame
  bool mDiagnosticsWasDrawn {false};// the frame before: the checks run again each time the tab comes into view
  bool mDiagnosticsTimer {false};
  ThreadUtil::BackgroundTask<std::vector<Diagnostics::SelfCheck>> mSelfTestTask;
  std::optional<std::vector<Diagnostics::SelfCheck>> mSelfTest;
  bool mSelfTestWanted {true};// run on the next frame that shows the tab
  uint64_t mLogVersion {~0ull};
  std::vector<Log::LogRing::Line> mLogLines;
  bool mLogScrollToEnd {true};
  // What the page last showed of the game, to repaint only on a change
  uint32_t mShownGame {0};
  uint32_t mShownSession {0};
  uint64_t mShownFrames {0};
  bool mOpenPrivacy {false};
  ThreadUtil::BackgroundTask<Diagnostics::BundleResult> mBundleTask;
  std::optional<Diagnostics::BundleResult> mBundleResult;
  std::filesystem::path mBundlePath;

  // Tests
  std::optional<std::filesystem::path> mScreenshot;
  bool mScreenshotWithPicker {false};
  bool mScreenshotTaken {false};
  uint64_t mIdleStartPaints {0};
  TimeUtil::TimePoint mOpenedAt {};
  bool mFirstPaintLogged {false};
  // Where opening's time went, for the first-frame log line (doc 08's
  // 50 ms: test round 5 measured 102-117 ms on its PC, 36-38 ms here)
  void MarkOpening(const char* part);
  TimeUtil::TimePoint mOpeningMark {};
  std::string mOpeningParts;
  int64_t mOpeningMs {-1};
  // Input and WM_PAINT messages, and the last of them, so the idle check
  // can tell its own painting from a mouse moved over the window
  uint64_t mInputs {0};
  uint64_t mIdleStartInputs {0};
  UINT mLastInput {0};
};

}// namespace OpenDesktopVR::Host
