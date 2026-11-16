// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// A test pattern drawn by the host (M2): proves the whole path from the
// host's GPU to the headset without capturing anything. The animated
// variant moves a bar and counts frames, so staleness and dropped updates
// are visible; the static one never changes, so it must cost nothing.
// Same corner colours as the layer's test card: a flipped panel is obvious.
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/GpuDevice.hpp>

#include <string>

namespace OpenDesktopVR::Render {

struct TestPatternOptions {
  std::wstring mTitle {L"Test pattern"};
  bool mAnimated {true};
  PixelSize mSize {960, 600};
  // Test only: every frame a different size, at most mSize (a window being
  // resized or trimmed), to check that nothing is re-created (RN-7)
  bool mResizing {false};
};

class TestPatternSource final : public ContentSource {
 public:
  TestPatternSource(GpuDevice& gpu, TestPatternOptions options);

  PixelSize Size() const override;
  bool IsAnimated() const override {
    return mOptions.mAnimated;
  }
  void Render(ID2D1DeviceContext2* context, PixelSize size, TimeUtil::TimePoint now) override;

  uint64_t FramesDrawn() const noexcept {
    return mFrames;
  }

 private:
  void CreateResources(ID2D1DeviceContext2* context);

  TestPatternOptions mOptions;
  winrt::com_ptr<IDWriteFactory> mDWrite;
  // Created once and cached (RN-8)
  winrt::com_ptr<IDWriteTextFormat> mTitleFormat;
  winrt::com_ptr<IDWriteTextFormat> mBodyFormat;
  winrt::com_ptr<ID2D1SolidColorBrush> mBrush;
  ID2D1DeviceContext2* mResourcesFor {nullptr};
  TimeUtil::TimePoint mStart {};
  uint64_t mFrames {0};
};

}// namespace OpenDesktopVR::Render
