// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// A desktop window as panel content, through Windows.Graphics.Capture
// (doc 04 "Window capture"):
// - found by a match rule; re-found about once a second while missing, and
//   no more once found (CP-6); a failed start is retried (CP-7);
// - each FrameArrived drains the pool to the newest frame (CP-1) and wakes
//   the render loop (CP-2); the frame is copied on the render thread, client
//   area only (CP-3), and compared on the GPU with the frame shown: an
//   identical frame is dropped (Windows 10 sends some; test round 4). Only
//   a still window waits for that check; a window that changed in the last
//   half second (a video) is drawn at once, while the check runs alongside
//   (CP-10: the check's answer comes late while a game fills the GPU);
// - the user's trim (CT-12, in pixels) is applied when drawing, so a new
//   trim shows at once, without waiting for the window to send a frame;
// - inactive (hidden, or nobody watching): frames are dropped, the session
//   stays, so showing again is instant (CP-5);
// - frame-pool resize on a content-size change; captured cursor off; the
//   capture border off where Windows allows it;
// - callbacks hold weak references (CP-7) and run under a catch-all (TH-8).
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/WindowMatch.hpp>

#include <memory>
#include <string>

namespace OpenDesktopVR::Capture {

struct WindowCaptureOptions {
  WindowMatch::Rule mRule;
  std::wstring mName;// shown while waiting for the window
};

class WindowCaptureSource final : public Render::ContentSource {
 public:
  WindowCaptureSource(Render::GpuDevice& gpu, WindowCaptureOptions options, Render::MarkDirty markDirty);
  ~WindowCaptureSource() override;

  Render::PixelSize Size() const override;
  Render::Readiness Prepare() override;
  std::string StatsText() const override;
  void Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint now) override;
  void SetActive(bool active) override;
  void SetCrop(const Render::Crop& crop) override;
  Render::PixelSize UntrimmedSize() const override;
  void Maintain(TimeUtil::TimePoint now) override;
  std::wstring Describe() const override;

  // For tests and stats
  uint64_t FramesArrived() const noexcept;
  uint64_t FramesCopied() const noexcept;
  uint64_t FramesUnchanged() const noexcept;
  // Drawn as soon as they arrived: the window was live (CP-10)
  uint64_t FramesDrawnAtOnce() const noexcept;
  // Hitches while a window keeps changing: a frame Windows composed much
  // later than the window's usual interval (the window's own), and a frame
  // we drew more than about half a frame later than the one before (ours)
  uint64_t WindowGaps() const noexcept;
  uint64_t LateDraws() const noexcept;
  bool IsCapturing() const noexcept;

 private:
  struct State;
  std::shared_ptr<State> mState;// shared with capture callbacks (weakly)
};

// Whether this Windows can capture windows at all
bool IsCaptureSupported() noexcept;

}// namespace OpenDesktopVR::Capture
