// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The keyboard panel's content (doc 07 "Rendering", ADR-036): draws what
// the KeyboardModel says with Direct2D, on the render thread.
//
// - Only on change; the panel's cap (30 a second, doc 08) paces hover and
//   the preview, and a key press is drawn at once (TakeUrgent).
// - Fades in and out over 150 ms; hidden once it has faded out, so the
//   layer stops showing it (IsHiddenNow).
// - The preview strip (K13): part of the target panel's newest frame,
//   around the caret, given just before each draw (PartOfPanel, SetPart).
// - Opacity while typing and otherwise (K9) on everything it draws.
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/KeyboardModel.hpp>

#include <winrt/base.h>

#include <memory>

namespace OpenDesktopVR::VirtualKeyboard {

class KeyboardSource final : public Render::ContentSource {
 public:
  static constexpr auto FadeTime = std::chrono::milliseconds(150);

  KeyboardSource(Render::GpuDevice& gpu, std::shared_ptr<KeyboardModel> model, Render::MarkDirty markDirty);
  ~KeyboardSource() override;
  KeyboardSource(const KeyboardSource&) = delete;
  KeyboardSource& operator=(const KeyboardSource&) = delete;

  Render::PixelSize Size() const override;
  bool IsAnimated() const override {
    return mAnimating;
  }
  void Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint now) override;
  bool IsHiddenNow() const override;
  bool TakeUrgent() override {
    return mModel->TakeUrgent();
  }
  uint64_t PartOfPanel() const override;
  void SetPart(ID2D1Bitmap1* frame, Render::PixelSize size) override {
    mPart = frame;
    mPartSize = size;
  }
  std::wstring Describe() const override;
  std::string StatsText() const override;

 private:
  void CreateResources(ID2D1DeviceContext2* context);
  void DrawKeys(ID2D1DeviceContext2* context, const KeyboardFrame& frame, TimeUtil::TimePoint now);
  void DrawStrip(ID2D1DeviceContext2* context, const KeyboardFrame& frame, TimeUtil::TimePoint now);
  void Fill(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& rect, uint32_t rgb);
  void Text(ID2D1DeviceContext2* context, const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
    uint32_t rgb);

  std::shared_ptr<KeyboardModel> mModel;
  winrt::com_ptr<IDWriteFactory> mDWrite;
  ID2D1DeviceContext2* mResourcesFor {nullptr};
  winrt::com_ptr<ID2D1SolidColorBrush> mBrush;
  winrt::com_ptr<IDWriteTextFormat> mLegend;// one character
  winrt::com_ptr<IDWriteTextFormat> mLabel;// words ("Enter", "Caps Lock")
  winrt::com_ptr<IDWriteTextFormat> mSmall;
  winrt::com_ptr<IDWriteTextFormat> mStrip;

  float mFade {0};
  float mAlpha {1};// this draw's opacity
  TimeUtil::TimePoint mLastRender {};
  bool mAnimating {false};
  ID2D1Bitmap1* mPart {nullptr};
  Render::PixelSize mPartSize {};
  uint64_t mRenders {0};
  uint64_t mPreviews {0};
};

}// namespace OpenDesktopVR::VirtualKeyboard
