// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The in-VR quick menu's logic (doc 09 "In-VR UI", CT-10, UI-6; ADR-041),
// with nothing drawn and no threads:
// - what each page holds and where (BuildLayout), in the menu panel's
//   pixels, at a fixed 2.5 pixels a millimetre, so every target is at
//   least 20 mm each way in VR (DU-6);
// - which target a fingertip is over (HitTest);
// - what a touch does (MenuPointer): a target acts when the finger lifts
//   off the same target it pressed; sliding off first cancels (DU-7);
// - where a command takes the menu (Navigate): pages and list scrolling.
// The Position page (test round 19) moves a panel or the keyboard with the
// same settings the settings window has: left and right, down and up,
// distance, facing, turn and tilt.
// Commands that change the host's state (show a panel, switch profile) are
// for the host's controller to run; this only says which.
// Closed, the menu is a 40 mm button that opens it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OpenDesktopVR::QuickMenu {

// 0.4 mm a pixel: big enough to read and tap, small enough to sit low
inline constexpr float PixelsPerMillimeter = 2.5f;
inline constexpr float MetersPerPixel = 0.001f / PixelsPerMillimeter;
// Doc 09 "Interaction": hand tracking isn't a mouse
inline constexpr float MinTargetMillimeters = 20.0f;
inline constexpr uint32_t Width = 640;// 256 mm
inline constexpr uint32_t ButtonSize = 100;// the closed menu: 40 mm
// A list (panels, windows, profiles) shows this many rows at a time
inline constexpr std::size_t RowsPerPage = 5;

// The Panel page's steps and limits (the host applies them)
inline constexpr float LevelStep = 0.1f;// opacity and brightness
inline constexpr float MinLevel = 0.1f; // never invisible
inline constexpr float SizeStepFactor = 1.1f;
inline constexpr float MinWidthMeters = 0.1f;
inline constexpr float MaxWidthMeters = 3.0f;

// Where panels can go, from where the head was at the last recenter: the
// settings window's limits too
inline constexpr float MaxAcrossMeters = 5.0f;// left or right
inline constexpr float MaxUpDownMeters = 3.0f;
inline constexpr float MinAwayMeters = 0.1f;  // distance ahead
inline constexpr float MaxAwayMeters = 10.0f;
inline constexpr float MaxTurnDegrees = 180.0f;
inline constexpr float MaxTiltDegrees = 90.0f;
// The Position page's steps, big and small
inline constexpr float StepMeters = 0.05f;
inline constexpr float SmallStepMeters = 0.01f;
inline constexpr float StepDegrees = 5.0f;
inline constexpr float SmallStepDegrees = 1.0f;

// The keyboard, as the Position page's target (the host's KeyboardPanelID)
inline constexpr uint64_t KeyboardID = 0x6B6579626F617264ull;

enum class Page : uint8_t { Home, Panel, Windows, Profiles, Settings, Position };

enum class CommandKind : uint8_t {
  None,
  // Where the menu goes
  Open,
  Close,
  Back,
  ShowPanelPage,// mArg: the panel's ID
  ShowWindows,  // for the Panel page's panel
  ShowPosition, // mArg: the panel's ID, or KeyboardID
  ShowProfiles,
  ShowSettings,
  PreviousRows, // a list's previous or next rows
  NextRows,
  Scroll,// mStep rows (two-finger scroll), negative toward the top
  // What the host does
  TogglePanel,// mArg: the panel's ID
  ToggleAll,
  Recenter,
  SwitchProfile,// mArg: the index in MenuData::mProfiles
  PickWindow,   // mArg: the index in MenuData::mWindows
  // The Panel page's panel (mArg), by mStep (-1 or +1)
  Opacity,
  Brightness,
  ToggleTint,
  Size,
  // The Position page's panel or keyboard (mArg), by mStep steps
  MoveAcross,// + to the right
  MoveUp,
  MoveAway,// + farther
  ToggleFaceMe,
  Turn,// + to its left, as the settings window's Turn
  Tilt,// + its top away
  ToggleSmallSteps,
  // Settings
  ToggleKeyboard,
  UpdateRate,// mStep
  ToggleHandWhilePointing,
  ToggleButton,
};

struct Command {
  CommandKind mKind {CommandKind::None};
  uint64_t mArg {0};
  int mStep {0};
  bool operator==(const Command&) const = default;
};

// For logs: "toggle panel", "opacity +1"
std::string Describe(const Command& command);

// What the menu shows, gathered by the host when something changes
struct PanelInfo {
  uint64_t mID {0};
  std::wstring mName;
  bool mWindow {false};// shows a window: "Change window"
  bool mShowing {true};
  float mOpacity {1.0f};
  float mBrightness {1.0f};
  bool mTint {false};
  float mWidthMeters {0.36f};
  // Where it is (the Position page)
  std::array<float, 3> mPosition {0, 0, -0.6f};
  bool mFaceOrigin {true};
  float mYawDegrees {0};
  float mPitchDegrees {0};
  bool operator==(const PanelInfo&) const = default;
};

struct ProfileInfo {
  std::string mID;
  std::wstring mName;
  bool mActive {false};
  bool operator==(const ProfileInfo&) const = default;
};

struct WindowEntry {
  std::wstring mTitle;
  std::wstring mApp;// "firefox"
  bool operator==(const WindowEntry&) const = default;
};

struct MenuData {
  std::vector<PanelInfo> mPanels;
  std::vector<ProfileInfo> mProfiles;
  std::wstring mProfileName;
  bool mAnyShowing {false};
  bool mKeyboard {true};
  // Where the keyboard is (its Position page); its ID is KeyboardID
  PanelInfo mKeyboardPlacement {.mID = KeyboardID, .mName = L"Keyboard"};
  int mUpdateRate {30};
  bool mSlowerRate {true};// a slower or faster update rate exists
  bool mFasterRate {true};
  bool mHandWhilePointing {true};
  bool mButton {true};
  // The Windows page's list; nullopt while it's being made
  std::optional<std::vector<WindowEntry>> mWindows;
  bool operator==(const MenuData&) const = default;
};

struct MenuState {
  bool mOpen {false};
  Page mPage {Page::Home};
  uint64_t mPanel {0};    // the Panel, Windows and Position pages' panel
  std::size_t mFirst {0}; // a list's first row shown
  bool mSmallSteps {false};// the Position page's
  bool operator==(const MenuState&) const = default;
};

// A rectangle in the menu's pixels
struct Rect {
  float mLeft {0};
  float mTop {0};
  float mRight {0};
  float mBottom {0};
  float Width() const noexcept {
    return mRight - mLeft;
  }
  float Height() const noexcept {
    return mBottom - mTop;
  }
  bool Contains(float x, float y) const noexcept {
    return x >= mLeft && x < mRight && y >= mTop && y < mBottom;
  }
  bool operator==(const Rect&) const = default;
};

enum class ItemKind : uint8_t {
  Title,  // the header's text
  Label,  // a row's name, with its value on the right
  Note,   // dim text
  Button, // a target with centred text
  Row,    // a target across a row: text, a value, an arrow if it opens a page
  Toggle, // a switch: on or off
  Stepper,// "-" or "+"
  Icon,   // the closed menu's button
};

struct Item {
  ItemKind mKind {ItemKind::Note};
  Rect mRect;
  std::wstring mText;
  std::wstring mValue;
  Command mCommand;// None: not a target
  bool mOn {false};// a toggle's state; a row's "chosen" mark
  bool mEnabled {true};
  bool operator==(const Item&) const = default;

  bool IsTarget() const noexcept {
    return mCommand.mKind != CommandKind::None && mEnabled;
  }
};

struct Layout {
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  std::vector<Item> mItems;
  bool operator==(const Layout&) const = default;
};

Layout BuildLayout(const MenuState& state, const MenuData& data);

// The target under (x, y), in the layout's pixels; -1 if none
int HitTest(const Layout& layout, float x, float y) noexcept;

// The state after a command. Host commands leave the page as it is, apart
// from switching profile (back to the start) and picking a window (back
// to its panel). Lists stay within their rows.
MenuState Navigate(MenuState state, const Command& command, const MenuData& data);

// How many rows the current page's list has
std::size_t ListLength(const MenuState& state, const MenuData& data) noexcept;

// A fingertip on the menu, from the hand thread's pointer
enum class Touch : uint8_t { Away, Hover, Down };

struct PointerResult {
  int mHover {-1};  // the target under the finger
  int mPressed {-1};// the target held, while the finger is still on it
  std::optional<Command> mActivated;
  int mScrollRows {0};// two-finger scroll, negative toward the top
};

class MenuPointer {
 public:
  // (u, v): 0 to 1 across the panel from its top left; `wheel` in
  // WM_MOUSEWHEEL units, positive when the content moves down
  PointerResult Update(const Layout& layout, float u, float v, Touch touch, int32_t wheel);
  // Tracking stopped, or the menu closed: let go without acting
  void Reset() noexcept;

 private:
  bool mDown {false};
  // What the press started on: matched by command, so a layout rebuilt
  // mid-press (a keybind hid a panel) can't make it act on another target
  std::optional<Command> mPressed;
  int32_t mWheel {0};
};

}// namespace OpenDesktopVR::QuickMenu
