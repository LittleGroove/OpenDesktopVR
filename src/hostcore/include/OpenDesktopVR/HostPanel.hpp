// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// A panel as the host sees it: where it is, what it shows, and its render
// state. Placement comes from a PanelDefinition, so where panels go is data,
// not code (settings will provide them from M3).
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/PanelRing.hpp>
#include <OpenDesktopVR/UpdatePolicy.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace OpenDesktopVR::Host {

struct PanelDefinition {
  uint64_t mID {0};
  std::wstring mName;
  // LOCAL space: seated, origin at the head when the view was last recentered
  float mPosition[3] {0, 0, -0.6f};
  // Turned about the vertical axis; by default the panel faces the origin
  bool mFaceOrigin {true};
  float mYawDegrees {0};
  float mWidthMeters {0.36f};// height follows the content's aspect
  double mMaxUpdatesPerSecond {0};// 0 = the global maximum
};

struct HostPanel {
  PanelDefinition mDefinition;
  std::unique_ptr<Render::ContentSource> mSource;
  std::unique_ptr<Render::PanelRing> mRing;
  Panels::UpdateRateLimiter mLimiter;
  Panels::DirtyFlag mDirty;
  bool mVisible {true};
  uint64_t mContentVersion {0};
  uint32_t mTextureGeneration {0};
  TimeUtil::TimePoint mLastPublish {};
  uint64_t mRenders {0};
};

// The M2 panels: an animated test pattern on the left, a static one on the
// right, both angled toward the viewer
std::vector<PanelDefinition> DefaultTestPanels();

// Orientation (x, y, z, w) for a definition
void PanelOrientation(const PanelDefinition& definition, float out[4]) noexcept;

}// namespace OpenDesktopVR::Host
