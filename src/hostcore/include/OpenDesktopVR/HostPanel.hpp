// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// A panel as the host sees it: where it is, what it shows, and its render
// state. Placement and content come from a PanelDefinition, stored in
// settings: where panels go and what they show is data, not code.
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/PanelRing.hpp>
#include <OpenDesktopVR/UpdatePolicy.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace OpenDesktopVR::Host {

enum class PanelKind { TestPattern, Window };

struct PanelDefinition {
  uint64_t mID {0};
  std::wstring mName;
  PanelKind mKind {PanelKind::TestPattern};
  bool mAnimated {true};          // test patterns
  WindowMatch::Rule mWindow;      // windows (ST-8: a rule, never an HWND)
  bool mEnabled {true};
  // LOCAL space: seated, origin at the head when the view was last recentered
  float mPosition[3] {0, 0, -0.6f};
  // Turned about the vertical axis; by default the panel faces the origin
  bool mFaceOrigin {true};
  float mYawDegrees {0};
  float mWidthMeters {0.36f};// height follows the content's aspect
  double mMaxUpdatesPerSecond {0};// 0 = the global maximum
  // Fields this version doesn't know, kept for a newer one (ST-5)
  JsonUtil::Json mExtra;

  // Same content (a new source isn't needed if only placement changed)
  bool SameContent(const PanelDefinition& other) const noexcept {
    return mKind == other.mKind && mAnimated == other.mAnimated && mWindow == other.mWindow;
  }
};

struct HostPanel {
  PanelDefinition mDefinition;
  std::unique_ptr<Render::ContentSource> mSource;
  std::unique_ptr<Render::PanelRing> mRing;
  Panels::UpdateRateLimiter mLimiter;
  // Shared with the source's "new content" callback, which may run on
  // another thread after the panel is gone
  std::shared_ptr<Panels::DirtyFlag> mDirty {std::make_shared<Panels::DirtyFlag>()};
  bool mActive {false};
  uint64_t mContentVersion {0};
  uint32_t mTextureGeneration {0};
  TimeUtil::TimePoint mLastPublish {};
  uint64_t mRenders {0};
};

// The panels a fresh install shows: an animated test pattern on the left, a
// static one on the right, both angled toward the viewer
std::vector<PanelDefinition> DefaultTestPanels();

// A new panel for a window, placed in the next free spot of a grid in front
// of the viewer
PanelDefinition NewWindowPanel(const WindowUtil::WindowInfo& window, std::size_t existingWindowPanels);

uint64_t NewPanelID() noexcept;

// Orientation (x, y, z, w) for a definition
void PanelOrientation(const PanelDefinition& definition, float out[4]) noexcept;

}// namespace OpenDesktopVR::Host
