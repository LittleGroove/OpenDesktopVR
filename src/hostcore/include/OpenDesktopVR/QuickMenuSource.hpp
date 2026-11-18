// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The quick menu panel's content (ADR-041): draws what the QuickMenuModel
// says with Direct2D and DirectWrite, on the render thread, only when it
// changes. Its size is the layout's (the panel's size in VR follows it at
// a fixed density); closed, it's the round button. Closed with the button
// turned off, it's hidden (IsHiddenNow), so the layer leaves it out.
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/QuickMenuModel.hpp>

#include <winrt/base.h>

#include <memory>

namespace OpenDesktopVR::Host {

class QuickMenuSource final : public Render::ContentSource {
 public:
  QuickMenuSource(Render::GpuDevice& gpu, std::shared_ptr<QuickMenuModel> model, Render::MarkDirty markDirty);
  // For drawing without a render loop (tests): a DirectWrite factory
  QuickMenuSource(IDWriteFactory* dwrite, std::shared_ptr<QuickMenuModel> model);
  ~QuickMenuSource() override;
  QuickMenuSource(const QuickMenuSource&) = delete;
  QuickMenuSource& operator=(const QuickMenuSource&) = delete;

  Render::PixelSize Size() const override;
  void Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint now) override;
  bool IsHiddenNow() const override;
  std::wstring Describe() const override;
  std::string StatsText() const override;

 private:
  void CreateResources(ID2D1DeviceContext2* context);
  void DrawItem(ID2D1DeviceContext2* context, const QuickMenu::Item& item, bool hover, bool pressed);
  void Fill(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& rect, uint32_t rgb, float alpha = 1.0f);
  void Text(ID2D1DeviceContext2* context, const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
    uint32_t rgb);
  // An arrow drawn as two strokes (a font's arrows sit small and low)
  void Chevron(ID2D1DeviceContext2* context, D2D1_POINT_2F centre, float half, bool right, uint32_t rgb, float width);

  std::shared_ptr<QuickMenuModel> mModel;
  winrt::com_ptr<IDWriteFactory> mDWrite;
  ID2D1DeviceContext2* mResourcesFor {nullptr};
  winrt::com_ptr<ID2D1SolidColorBrush> mBrush;
  winrt::com_ptr<ID2D1StrokeStyle> mRound;
  winrt::com_ptr<IDWriteTextFormat> mTitle;
  winrt::com_ptr<IDWriteTextFormat> mLeft;  // row and label text
  winrt::com_ptr<IDWriteTextFormat> mCentre;// buttons
  winrt::com_ptr<IDWriteTextFormat> mValue; // a row's value, on the right
  winrt::com_ptr<IDWriteTextFormat> mNote;
  winrt::com_ptr<IDWriteTextFormat> mSymbol;// back, close, steppers, arrows
  uint64_t mRenders {0};
};

}// namespace OpenDesktopVR::Host
