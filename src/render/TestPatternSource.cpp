// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/TestPatternSource.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace OpenDesktopVR::Render {

namespace {

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1) {
  return {
    static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
    static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
    static_cast<float>(rgb & 0xFF) / 255.0f,
    alpha};
}

// The product's palette: dark slate, with the test card's corner colours
constexpr uint32_t Background = 0x1E2430;
constexpr uint32_t Grid = 0x3A4458;
constexpr uint32_t Text = 0xF0F0F0;
constexpr uint32_t Accent = 0xF0B040;
constexpr uint32_t Corners[4] {0xE04848, 0x48C060, 0x4878E0, 0xF0F0F0};

}// namespace

TestPatternSource::TestPatternSource(GpuDevice& gpu, TestPatternOptions options)
  : mOptions(std::move(options)), mStart(TimeUtil::Now()) {
  mDWrite.copy_from(gpu.DWrite());
}

void TestPatternSource::CreateResources(ID2D1DeviceContext2* context) {
  mBrush = nullptr;
  context->CreateSolidColorBrush(Rgb(Text), mBrush.put());
  // Sizes scale with the panel; never 0 on a tiny panel (RN-8)
  const float scale = std::max(static_cast<float>(mOptions.mSize.mHeight) / 600.0f, 0.25f);
  mTitleFormat = nullptr;
  mBodyFormat = nullptr;
  mDWrite->CreateTextFormat(
    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
    std::max(48.0f * scale, 6.0f), L"", mTitleFormat.put());
  mDWrite->CreateTextFormat(
    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
    std::max(26.0f * scale, 6.0f), L"", mBodyFormat.put());
  mResourcesFor = context;
}

PixelSize TestPatternSource::Size() const {
  if (!mOptions.mResizing) {
    return mOptions.mSize;
  }
  // Cycles through four sizes, from full down to five eighths
  const auto step = static_cast<uint32_t>(mFrames % 4);
  return {mOptions.mSize.mWidth - step * mOptions.mSize.mWidth / 8, mOptions.mSize.mHeight - step * mOptions.mSize.mHeight / 8};
}

void TestPatternSource::Render(ID2D1DeviceContext2* context, PixelSize size, TimeUtil::TimePoint now) {
  if (mResourcesFor != context || !mBrush) {
    CreateResources(context);
  }
  ++mFrames;
  const float w = static_cast<float>(size.mWidth);
  const float h = static_cast<float>(size.mHeight);
  const float unit = std::min(w, h) / 12;

  context->Clear(Rgb(Background));

  // Grid
  mBrush->SetColor(Rgb(Grid));
  for (float x = 0; x < w; x += unit) {
    context->DrawLine({x, 0}, {x, h}, mBrush.get(), 1.5f);
  }
  for (float y = 0; y < h; y += unit) {
    context->DrawLine({0, y}, {w, y}, mBrush.get(), 1.5f);
  }

  // Corners: top-left red, top-right green, bottom-left blue, bottom-right white
  const float c = unit * 1.2f;
  const D2D1_RECT_F corners[4] {{0, 0, c, c}, {w - c, 0, w, c}, {0, h - c, c, h}, {w - c, h - c, w, h}};
  for (int i = 0; i < 4; ++i) {
    mBrush->SetColor(Rgb(Corners[i]));
    context->FillRectangle(corners[i], mBrush.get());
  }

  // Title and theme line
  mBrush->SetColor(Rgb(Text));
  const auto title = std::wstring {Product::DisplayName};
  context->DrawText(title.c_str(), static_cast<UINT32>(title.size()), mTitleFormat.get(),
    {c + unit * 0.5f, unit * 0.5f, w - c, unit * 2.2f}, mBrush.get());
  const auto line = std::format(L"{}  ·  {}", Product::Tagline, mOptions.mTitle);
  context->DrawText(line.c_str(), static_cast<UINT32>(line.size()), mBodyFormat.get(),
    {c + unit * 0.5f, unit * 2.2f, w - c, unit * 3.4f}, mBrush.get());

  // Animated: a bar sweeping across once a second, and a frame counter
  const double seconds = std::chrono::duration<double>(now - mStart).count();
  if (mOptions.mAnimated) {
    const float phase = static_cast<float>(seconds - std::floor(seconds));
    const float x = c + phase * (w - 2 * c - unit);
    mBrush->SetColor(Rgb(Accent));
    context->FillRectangle({x, h * 0.55f, x + unit, h * 0.55f + unit * 1.5f}, mBrush.get());
  } else {
    mBrush->SetColor(Rgb(Accent));
    context->DrawEllipse({{w / 2, h * 0.62f}, unit * 1.2f, unit * 1.2f}, mBrush.get(), unit * 0.2f);
    context->FillEllipse({{w / 2, h * 0.62f}, unit * 0.4f, unit * 0.4f}, mBrush.get());
  }
  mBrush->SetColor(Rgb(Text));
  const auto counter = mOptions.mAnimated
    ? std::format(L"frame {}  ·  {:.1f} s", mFrames, seconds)
    : std::wstring {L"static: drawn once"};
  context->DrawText(counter.c_str(), static_cast<UINT32>(counter.size()), mBodyFormat.get(),
    {c + unit * 0.5f, h - c - unit * 1.4f, w - c, h - c}, mBrush.get());
}

}// namespace OpenDesktopVR::Render
