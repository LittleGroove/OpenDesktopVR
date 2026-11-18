// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// A panel as the host sees it: where it is, what it shows, and its render
// state. Placement and content come from a PanelDefinition, stored in
// settings: where panels go and what they show is data, not code.
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/Keybinds.hpp>
#include <OpenDesktopVR/PanelRing.hpp>
#include <OpenDesktopVR/UpdatePolicy.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace OpenDesktopVR::Host {

enum class PanelKind { TestPattern, Window, Keyboard, QuickMenu };

struct PanelDefinition {
  uint64_t mID {0};
  std::wstring mName;
  PanelKind mKind {PanelKind::TestPattern};
  bool mAnimated {true};          // test patterns
  WindowMatch::Rule mWindow;      // windows (ST-8: a rule, never an HWND)
  bool mEnabled {true};
  // LOCAL space: seated, origin at the head when the view was last recentered
  float mPosition[3] {0, 0, -0.6f};
  // By default the panel faces the origin (turned and tilted toward it).
  // Otherwise it's turned by mYawDegrees about the vertical axis (positive
  // turns it to its left), then tilted by mPitchDegrees (positive tilts its
  // top away, so it faces up: for a panel below the eyes).
  bool mFaceOrigin {true};
  float mYawDegrees {0};
  float mPitchDegrees {0};
  float mWidthMeters {0.36f};// height follows the content's aspect...
  // ... up to this (PN-14: a box; 0 for none), the width then shrinking to
  // keep the aspect
  float mMaxHeightMeters {0};
  // Built-in menus (the quick menu, ADR-041): a fixed size per content
  // pixel instead, so the panel grows and shrinks with its content and
  // its targets keep their size in VR (DU-6). 0: the width above.
  float mMetersPerPixel {0};
  double mMaxUpdatesPerSecond {0};// 0 = the global maximum
  Render::Crop mCrop;// trimmed edges in pixels (CT-12)
  // How it looks (PN-4, PN-5), applied when the host draws it (doc 08)
  float mOpacity {1.0f};       // 0.1 to 1
  float mFocusedOpacity {1.0f};// while looked at (gaze focus, doc 05)
  float mBrightness {1.0f};    // 0.1 to 1
  bool mTintEnabled {false};
  uint32_t mTintColor {DefaultTintColor};// 0xRRGGBB
  // Windows: a Chromium-based app or Firefox keeps updating while the game
  // covers it, with the app helper loaded into it (CT-3, Q10, ADR-031). On
  // by default; changing it doesn't restart the capture.
  bool mKeepUpdatingWhenCovered {true};
  // Hands (doc 05, doc 09 "Interaction"): tap, hold and drag with a
  // pointing finger; scroll with two fingers
  bool mTouch {true};
  bool mScroll {true};
  // Its keybinds (UI-3), indexed like Keybinds::PanelActions
  std::array<Keybinds::Slots, Keybinds::PanelActions.size()> mKeybinds {};
  // Fields this version doesn't know, kept for a newer one (ST-5)
  JsonUtil::Json mExtra;

  static constexpr uint32_t DefaultTintColor = 0xFF5A40;// a warm red, for night flying
  static constexpr float MinimumLevel = 0.1f;// opacity and brightness: never invisible

  // Same content (a new source isn't needed if only placement changed)
  bool SameContent(const PanelDefinition& other) const noexcept {
    return mKind == other.mKind && mAnimated == other.mAnimated && mWindow == other.mWindow;
  }
  // Same pixels apart from the content (a change means a redraw)
  bool SameLook(const PanelDefinition& other) const noexcept {
    return mOpacity == other.mOpacity && mFocusedOpacity == other.mFocusedOpacity && mBrightness == other.mBrightness
      && mTintEnabled == other.mTintEnabled && mTintColor == other.mTintColor;
  }
  // No change to the content's pixels: drawn as it is
  bool IsPlain(bool focused) const noexcept {
    return (focused ? mFocusedOpacity : mOpacity) >= 1.0f && mBrightness >= 1.0f && !mTintEnabled;
  }
  const Keybinds::Slots& KeybindsFor(Keybinds::ActionKind kind) const;
  Keybinds::Slots& KeybindsFor(Keybinds::ActionKind kind);
};

// The colour matrix's diagonal (red, green, blue, alpha multipliers) for a
// panel's brightness, tint and opacity: the host applies it to the
// content's pixels (premultiplied alpha)
std::array<float, 4> LookMultipliers(const PanelDefinition& definition, bool focused) noexcept;

struct HostPanel {
  PanelDefinition mDefinition;
  std::unique_ptr<Render::ContentSource> mSource;
  std::unique_ptr<Render::PanelRing> mRing;
  Panels::UpdateRateLimiter mLimiter;
  // Shared with the source's "new content" callback, which may run on
  // another thread after the panel is gone
  std::shared_ptr<Panels::DirtyFlag> mDirty {std::make_shared<Panels::DirtyFlag>()};
  bool mActive {false};
  bool mHidden {false};// runtime only (PN-6): kept, but not drawn or shown
  // The content hides itself for now (the keyboard, ADR-036)
  bool mSourceHidden {false};
  // The content drawn before its look is applied (when it isn't plain)
  winrt::com_ptr<ID2D1Bitmap1> mLookTarget;
  uint64_t mContentVersion {0};
  uint32_t mTextureGeneration {0};
  TimeUtil::TimePoint mLastPublish {};
  uint64_t mRenders {0};
  uint64_t mRendersAtLastStats {0};
};

// The panels a fresh install shows: an animated test pattern on the left, a
// static one on the right, both angled toward the viewer
std::vector<PanelDefinition> DefaultTestPanels();

// Tests (doc 11, "Two panels overlapping in depth"): the two test patterns,
// one in front of the other and overlapping it as the viewer sees them,
// listed nearest first, so the layer has to put them back to front
std::vector<PanelDefinition> OverlappingTestPanels();

// A new panel for a window, placed in the next free spot of a grid in front
// of the viewer
PanelDefinition NewWindowPanel(const WindowUtil::WindowInfo& window, std::size_t existingWindowPanels);

uint64_t NewPanelID() noexcept;

// The turn and tilt that face the origin from `position`, in degrees
struct Facing {
  float mYawDegrees {0};
  float mPitchDegrees {0};
};
Facing FacingOrigin(const float position[3]) noexcept;

// A panel's size in VR for content of `contentWidth` x `contentHeight`
// pixels: its width, and the height its aspect gives, both shrunk to fit
// its maximum height if it has one (PN-14)
struct PanelSize {
  float mWidth {0};
  float mHeight {0};
};
PanelSize PanelSizeFor(const PanelDefinition& definition, uint32_t contentWidth, uint32_t contentHeight) noexcept;

// Orientation (x, y, z, w) for a definition
void PanelOrientation(const PanelDefinition& definition, float out[4]) noexcept;

}// namespace OpenDesktopVR::Host
