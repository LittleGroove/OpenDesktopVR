// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/QuickMenuSource.hpp>

#include <algorithm>
#include <format>

namespace OpenDesktopVR::Host {

namespace {

using QuickMenu::CommandKind;
using QuickMenu::ItemKind;

// Colours, 0xRRGGBB: the keyboard's, so the built-in panels look alike
constexpr uint32_t Background = 0x1B1D22;
constexpr uint32_t Control = 0x2E3138;
constexpr uint32_t ControlHover = 0x434852;
constexpr uint32_t ControlPressed = 0x3D6FCC;
constexpr uint32_t ControlDisabled = 0x23262B;
constexpr uint32_t ToggleOn = 0x2F9E6A;
constexpr uint32_t ToggleOnHover = 0x3DB57D;
constexpr uint32_t ToggleOff = 0x4A4F59;
constexpr uint32_t ToggleOffHover = 0x5C626D;
constexpr uint32_t Knob = 0xF1F3F4;
constexpr uint32_t TextColour = 0xE8EAED;
constexpr uint32_t DimText = 0x9AA0A6;
constexpr uint32_t DisabledText = 0x5F646C;
constexpr uint32_t ChosenMark = 0x5BE08A;
constexpr uint32_t Ring = 0x4C8DFF;

// In the menu's pixels
constexpr float Radius = 12;
constexpr float TextInset = 18;
constexpr float ArrowSpace = 44;
constexpr float ValueSpace = 170;

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f) {
  return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
    static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

D2D1_RECT_F ToD2D(const QuickMenu::Rect& rect) {
  return {rect.mLeft, rect.mTop, rect.mRight, rect.mBottom};
}

bool OpensPage(CommandKind kind) {
  return kind == CommandKind::ShowPanelPage || kind == CommandKind::ShowWindows || kind == CommandKind::ShowProfiles
    || kind == CommandKind::ShowSettings || kind == CommandKind::ShowPosition;
}

}// namespace

QuickMenuSource::QuickMenuSource(Render::GpuDevice& gpu, std::shared_ptr<QuickMenuModel> model, Render::MarkDirty markDirty)
  : mModel(std::move(model)) {
  mDWrite.copy_from(gpu.DWrite());
  mModel->SetWake(std::move(markDirty));
}

QuickMenuSource::QuickMenuSource(IDWriteFactory* dwrite, std::shared_ptr<QuickMenuModel> model) : mModel(std::move(model)) {
  mDWrite.copy_from(dwrite);
}

QuickMenuSource::~QuickMenuSource() {
  mModel->SetWake({});
}

Render::PixelSize QuickMenuSource::Size() const {
  const auto layout = mModel->Layout();
  if (!layout || layout->mWidth == 0 || layout->mHeight == 0) {
    return {QuickMenu::ButtonSize, QuickMenu::ButtonSize};
  }
  return {layout->mWidth, layout->mHeight};
}

bool QuickMenuSource::IsHiddenNow() const {
  return mModel->Frame().mHidden;
}

std::wstring QuickMenuSource::Describe() const {
  const auto frame = mModel->Frame();
  if (frame.mHidden) {
    return L"hidden";
  }
  return frame.mLayout && frame.mLayout->mWidth > QuickMenu::ButtonSize ? L"open" : L"closed";
}

std::string QuickMenuSource::StatsText() const {
  return std::format("quick menu: {} draws", mRenders);
}

void QuickMenuSource::CreateResources(ID2D1DeviceContext2* context) {
  mBrush = nullptr;
  context->CreateSolidColorBrush(Rgb(TextColour), mBrush.put());
  mRound = nullptr;
  winrt::com_ptr<ID2D1Factory> factory;
  context->GetFactory(factory.put());
  if (factory) {
    const D2D1_STROKE_STYLE_PROPERTIES round {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
      D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f};
    factory->CreateStrokeStyle(round, nullptr, 0, mRound.put());
  }
  const auto format = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT alignment, bool wrap,
                        winrt::com_ptr<IDWriteTextFormat>& out) {
    out = nullptr;
    if (FAILED(mDWrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          size, L"", out.put()))) {
      return;
    }
    out->SetTextAlignment(alignment);
    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
      // Long names end in "..." rather than running into the next thing
      const DWRITE_TRIMMING trimming {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      winrt::com_ptr<IDWriteInlineObject> ellipsis;
      if (SUCCEEDED(mDWrite->CreateEllipsisTrimmingSign(out.get(), ellipsis.put()))) {
        out->SetTrimming(&trimming, ellipsis.get());
      }
    }
  };
  format(28, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, mTitle);
  format(25, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, mLeft);
  format(25, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false, mCentre);
  format(22, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, false, mValue);
  format(22, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true, mNote);
  format(36, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false, mSymbol);
  mResourcesFor = context;
}

void QuickMenuSource::Fill(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& rect, uint32_t rgb, float alpha) {
  mBrush->SetColor(Rgb(rgb, alpha));
  context->FillRoundedRectangle(rect, mBrush.get());
}

void QuickMenuSource::Text(
  ID2D1DeviceContext2* context, const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, uint32_t rgb) {
  if (text.empty() || !format || rect.right <= rect.left) {
    return;
  }
  mBrush->SetColor(Rgb(rgb));
  context->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, mBrush.get(),
    D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void QuickMenuSource::Chevron(
  ID2D1DeviceContext2* context, D2D1_POINT_2F centre, float half, bool right, uint32_t rgb, float width) {
  const float dx = (right ? 0.5f : -0.5f) * half;
  const D2D1_POINT_2F tip {centre.x + dx, centre.y};
  mBrush->SetColor(Rgb(rgb));
  context->DrawLine({centre.x - dx, centre.y - half}, tip, mBrush.get(), width, mRound.get());
  context->DrawLine(tip, {centre.x - dx, centre.y + half}, mBrush.get(), width, mRound.get());
}

void QuickMenuSource::DrawItem(ID2D1DeviceContext2* context, const QuickMenu::Item& item, bool hover, bool pressed) {
  const auto rect = ToD2D(item.mRect);
  const auto controlColour = [&] {
    if (!item.mEnabled) {
      return ControlDisabled;
    }
    return pressed ? ControlPressed : hover ? ControlHover : Control;
  };
  const uint32_t text = item.mEnabled ? TextColour : DisabledText;
  switch (item.mKind) {
    case ItemKind::Title:
      Text(context, item.mText, mTitle.get(), rect, TextColour);
      return;
    case ItemKind::Label:
      Text(context, item.mText, mLeft.get(), {rect.left, rect.top, rect.right - ValueSpace, rect.bottom}, TextColour);
      Text(context, item.mValue, mValue.get(), {rect.right - ValueSpace, rect.top, rect.right - 8, rect.bottom}, DimText);
      return;
    case ItemKind::Note:
      Text(context, item.mText, mNote.get(), rect, DimText);
      return;
    case ItemKind::Button:
    case ItemKind::Stepper: {
      Fill(context, {rect, Radius, Radius}, controlColour());
      if (item.mCommand.mKind == CommandKind::Back) {
        Chevron(context, {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}, 12, false, text, 4.5f);
        return;
      }
      // One character (close, minus, plus) is a symbol, bigger
      Text(context, item.mText, item.mText.size() == 1 ? mSymbol.get() : mCentre.get(), rect, text);
      return;
    }
    case ItemKind::Row: {
      Fill(context, {rect, Radius, Radius}, controlColour());
      const bool arrow = OpensPage(item.mCommand.mKind);
      const float marks = (arrow || item.mOn) ? ArrowSpace : 0;
      const float value = item.mValue.empty() ? 0 : ValueSpace;
      Text(context, item.mText, mLeft.get(), {rect.left + TextInset, rect.top, rect.right - marks - value - 8, rect.bottom}, text);
      Text(context, item.mValue, mValue.get(), {rect.right - marks - value, rect.top, rect.right - marks - 8, rect.bottom}, DimText);
      const D2D1_RECT_F markRect {rect.right - ArrowSpace, rect.top, rect.right - 4, rect.bottom};
      if (arrow) {
        Chevron(context, {(markRect.left + markRect.right) / 2, (rect.top + rect.bottom) / 2}, 9, true, DimText, 3.5f);
      } else if (item.mOn) {
        Text(context, L"✓", mCentre.get(), markRect, ChosenMark);
      }
      return;
    }
    case ItemKind::Toggle: {
      // A switch filling most of its target: the knob on the right when on
      const float height = std::min(44.0f, rect.bottom - rect.top - 12);
      const float width = std::min(rect.right - rect.left - 20, 2.2f * height);
      const float cx = (rect.left + rect.right) / 2;
      const float cy = (rect.top + rect.bottom) / 2;
      const D2D1_RECT_F track {cx - (width / 2), cy - (height / 2), cx + (width / 2), cy + (height / 2)};
      uint32_t colour = item.mOn ? (hover ? ToggleOnHover : ToggleOn) : (hover ? ToggleOffHover : ToggleOff);
      if (pressed) {
        colour = ControlPressed;
      }
      if (hover || pressed) {
        Fill(context, {rect, Radius, Radius}, Control);
      }
      Fill(context, {track, height / 2, height / 2}, colour);
      const float knob = (height / 2) - 4;
      const float kx = item.mOn ? track.right - (height / 2) : track.left + (height / 2);
      mBrush->SetColor(Rgb(Knob));
      context->FillEllipse(D2D1::Ellipse({kx, cy}, knob, knob), mBrush.get());
      return;
    }
    case ItemKind::Icon: {
      // The closed menu: a round button with three lines
      const float cx = (rect.left + rect.right) / 2;
      const float cy = (rect.top + rect.bottom) / 2;
      const float r = std::min(rect.right - rect.left, rect.bottom - rect.top) / 2 - 3;
      mBrush->SetColor(Rgb(pressed ? ControlPressed : hover ? ControlHover : Background, 0.92f));
      context->FillEllipse(D2D1::Ellipse({cx, cy}, r, r), mBrush.get());
      mBrush->SetColor(Rgb(Ring, hover || pressed ? 1.0f : 0.7f));
      context->DrawEllipse(D2D1::Ellipse({cx, cy}, r - 2, r - 2), mBrush.get(), hover ? 4.0f : 3.0f);
      const float half = r * 0.42f;
      const float spacing = r * 0.30f;
      const float thickness = std::max(4.0f, r * 0.11f);
      mBrush->SetColor(Rgb(TextColour));
      for (int line = -1; line <= 1; ++line) {
        const float y = cy + (static_cast<float>(line) * spacing);
        context->FillRoundedRectangle(
          {{cx - half, y - (thickness / 2), cx + half, y + (thickness / 2)}, thickness / 2, thickness / 2}, mBrush.get());
      }
      return;
    }
  }
}

void QuickMenuSource::Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint) {
  if (mResourcesFor != context || !mBrush) {
    CreateResources(context);
  }
  context->Clear(D2D1_COLOR_F {0, 0, 0, 0});
  ++mRenders;
  const auto frame = mModel->Frame();
  if (!frame.mLayout || !mBrush) {
    return;
  }
  const auto& layout = *frame.mLayout;
  const bool open = layout.mWidth > QuickMenu::ButtonSize;
  if (open) {
    const D2D1_RECT_F all {0, 0, static_cast<float>(size.mWidth), static_cast<float>(size.mHeight)};
    Fill(context, {all, 18, 18}, Background);
  }
  context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);// RN-13
  for (std::size_t i = 0; i < layout.mItems.size(); ++i) {
    const auto& item = layout.mItems[i];
    const bool target = item.IsTarget();
    const auto index = static_cast<int>(i);
    DrawItem(context, item, target && index == frame.mHover, target && index == frame.mPressed);
  }
}

}// namespace OpenDesktopVR::Host
