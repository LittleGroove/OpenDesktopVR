// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/KeyboardSource.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace OpenDesktopVR::VirtualKeyboard {

namespace {

constexpr float U = PixelsPerUnit;

// Colours, 0xRRGGBB
constexpr uint32_t Background = 0x1B1D22;
constexpr uint32_t StripBackground = 0x24272D;
constexpr uint32_t KeyColour = 0x363A42;
constexpr uint32_t ActionColour = 0x2B2E35;
constexpr uint32_t HoverColour = 0x55606E;
constexpr uint32_t FlashColour = 0x4C8DFF;
constexpr uint32_t HeldColour = 0x3D6FCC;
constexpr uint32_t LatchedColour = 0x2F7A58;
constexpr uint32_t LockedColour = 0x3FAA78;
constexpr uint32_t LightColour = 0x5BE08A;
constexpr uint32_t TextColour = 0xE8EAED;
constexpr uint32_t DimText = 0x9AA0A6;
constexpr uint32_t DeadText = 0xFFB74D;
constexpr uint32_t WarnText = 0xFFB74D;

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f) {
  return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
    static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

uint32_t Mix(uint32_t a, uint32_t b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  uint32_t out = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const float ca = static_cast<float>((a >> shift) & 0xFF);
    const float cb = static_cast<float>((b >> shift) & 0xFF);
    out |= static_cast<uint32_t>(std::lround(ca + ((cb - ca) * t))) << shift;
  }
  return out;
}

D2D1_RECT_F Pixels(const KeyRect& rect, float inset = 0) {
  return {(rect.mX * U) + inset, (rect.mY * U) + inset, ((rect.mX + rect.mWidth) * U) - inset,
    ((rect.mY + rect.mHeight) * U) - inset};
}

}// namespace

KeyboardSource::KeyboardSource(Render::GpuDevice& gpu, std::shared_ptr<KeyboardModel> model, Render::MarkDirty markDirty)
  : mModel(std::move(model)) {
  mDWrite.copy_from(gpu.DWrite());
  mModel->SetWake(std::move(markDirty));
}

KeyboardSource::~KeyboardSource() {
  mModel->SetWake({});
}

Render::PixelSize KeyboardSource::Size() const {
  const auto frame = mModel->Frame();
  const auto geometry = frame.mFaces ? frame.mFaces->mGeometry : nullptr;
  if (!geometry) {
    // Before the first layouts arrive: the full keyboard's size
    static const auto fallback = BuildGeometry({});
    return {static_cast<uint32_t>(std::lround(fallback.mWidth * U)), static_cast<uint32_t>(std::lround(fallback.mHeight * U))};
  }
  return {static_cast<uint32_t>(std::lround(geometry->mWidth * U)), static_cast<uint32_t>(std::lround(geometry->mHeight * U))};
}

bool KeyboardSource::IsHiddenNow() const {
  return mFade <= 0.0f && !mModel->IsShown();
}

uint64_t KeyboardSource::PartOfPanel() const {
  const auto frame = mModel->Frame();
  return (frame.mShown && frame.mPreviewCaret) ? frame.mPreviewPanel : 0;
}

std::wstring KeyboardSource::Describe() const {
  return mModel->IsShown() ? L"showing" : L"hidden";
}

std::string KeyboardSource::StatsText() const {
  return std::format("keyboard: {} draws, {} with the preview", mRenders, mPreviews);
}

void KeyboardSource::CreateResources(ID2D1DeviceContext2* context) {
  mBrush = nullptr;
  context->CreateSolidColorBrush(Rgb(TextColour), mBrush.put());
  const auto format = [&](float size, DWRITE_FONT_WEIGHT weight, winrt::com_ptr<IDWriteTextFormat>& out) {
    out = nullptr;
    if (SUCCEEDED(mDWrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, size, L"", out.put()))) {
      out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
  };
  format(0.42f * U, DWRITE_FONT_WEIGHT_SEMI_BOLD, mLegend);
  format(0.25f * U, DWRITE_FONT_WEIGHT_SEMI_BOLD, mLabel);
  format(0.22f * U, DWRITE_FONT_WEIGHT_NORMAL, mSmall);
  format(0.34f * U, DWRITE_FONT_WEIGHT_SEMI_BOLD, mStrip);
  mResourcesFor = context;
}

void KeyboardSource::Fill(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& rect, uint32_t rgb) {
  mBrush->SetColor(Rgb(rgb));
  mBrush->SetOpacity(mAlpha);
  context->FillRoundedRectangle(rect, mBrush.get());
}

void KeyboardSource::Text(
  ID2D1DeviceContext2* context, const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, uint32_t rgb) {
  if (text.empty() || !format) {
    return;
  }
  mBrush->SetColor(Rgb(rgb));
  mBrush->SetOpacity(mAlpha);
  context->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, mBrush.get(),
    D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void KeyboardSource::Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint now) {
  if (context != mResourcesFor || !mBrush) {
    CreateResources(context);
  }
  const auto frame = mModel->Frame();
  // The fade (doc 07): toward shown or hidden, in steps of at least a
  // quarter, so a capped panel still fades in about 150 ms
  const float target = frame.mShown ? 1.0f : 0.0f;
  // (a first draw after a pause takes the smallest step)
  const double since = mLastRender == TimeUtil::TimePoint {} ? 1.0 : std::chrono::duration<double>(now - mLastRender).count();
  const float step = since > 0.1
    ? 0.25f
    : std::max(0.25f, static_cast<float>(since / std::chrono::duration<double>(FadeTime).count()));
  mFade = target > mFade ? std::min(target, mFade + step) : std::max(target, mFade - step);
  mLastRender = now;
  mAlpha = mFade * (frame.mTyping ? frame.mTypingOpacity : frame.mIdleOpacity);
  ++mRenders;

  context->Clear(D2D1_COLOR_F {0, 0, 0, 0});
  const float w = static_cast<float>(size.mWidth);
  const float h = static_cast<float>(size.mHeight);
  Fill(context, {{0, 0, w, h}, 0.25f * U, 0.25f * U}, Background);
  if (frame.mFaces && frame.mFaces->mGeometry) {
    DrawStrip(context, frame, now);
    DrawKeys(context, frame, now);
  }

  // Keep drawing while something changes over time: the fade, a flash
  bool flashing = now < frame.mNoTargetUntil;
  for (std::size_t i = 0; i < KeyboardFrame::MaxFlashes; ++i) {
    flashing = flashing || (frame.mFlashKey[i] >= 0 && now < frame.mFlashUntil[i]);
  }
  mAnimating = mFade != target || flashing;
}

void KeyboardSource::DrawStrip(ID2D1DeviceContext2* context, const KeyboardFrame& frame, TimeUtil::TimePoint now) {
  const auto& geometry = *frame.mFaces->mGeometry;
  Fill(context, {Pixels(geometry.mTopStrip, 2), 0.12f * U, 0.12f * U}, StripBackground);

  // Where keys go, or what to do first
  auto target = Pixels(geometry.mTargetArea, 0.1f * U);
  if (frame.mTarget.empty()) {
    const bool warn = now < frame.mNoTargetUntil;
    Text(context, L"Tap a text field first", mLabel.get(), target, warn ? WarnText : DimText);
  } else {
    Text(context, frame.mTarget, mStrip.get(), target, TextColour);
  }

  auto preview = Pixels(geometry.mPreviewArea, 0.12f * U);
  if (frame.mFaces->mPendingDeadKey) {
    // An accent waiting for the next key
    D2D1_RECT_F accent {preview.left, preview.top, preview.left + (1.2f * U), preview.bottom};
    Text(context, std::wstring(1, *frame.mFaces->mPendingDeadKey) + L" …", mStrip.get(), accent, DeadText);
    preview.left += 1.4f * U;
  }
  if (!mPart || !frame.mPreviewCaret || mPartSize.mWidth == 0 || mPartSize.mHeight == 0) {
    if (!frame.mTarget.empty()) {
      Text(context, L"What you type shows here", mLabel.get(), preview, DimText);
    }
    return;
  }
  // A strip of the target around the caret, a line of text high, the caret
  // toward its right (text grows rightward); kept inside the frame
  const auto& caret = *frame.mPreviewCaret;
  const float caretHeight = static_cast<float>(std::max<LONG>(caret.bottom - caret.top, 8));
  const float aspect = (preview.right - preview.left) / std::max(1.0f, preview.bottom - preview.top);
  const float frameW = static_cast<float>(mPartSize.mWidth);
  const float frameH = static_cast<float>(mPartSize.mHeight);
  float srcH = std::clamp(caretHeight * 2.2f, 20.0f, std::min(160.0f, frameH));
  float srcW = std::min(srcH * aspect, frameW);
  srcH = std::min(srcH, srcW / aspect);
  const float caretX = static_cast<float>(caret.left);
  const float caretY = static_cast<float>(caret.top + caret.bottom) / 2.0f;
  const float left = std::clamp(caretX - (srcW * 0.75f), 0.0f, std::max(0.0f, frameW - srcW));
  const float top = std::clamp(caretY - (srcH / 2), 0.0f, std::max(0.0f, frameH - srcH));
  const D2D1_RECT_F source {left, top, left + srcW, top + srcH};
  // Letterboxed to keep the text's shape
  const float drawnW = (preview.bottom - preview.top) * (srcW / srcH);
  const D2D1_RECT_F destination {preview.left, preview.top, std::min(preview.right, preview.left + drawnW), preview.bottom};
  context->DrawBitmap(mPart, destination, mAlpha, D2D1_INTERPOLATION_MODE_LINEAR, &source);
  ++mPreviews;
}

void KeyboardSource::DrawKeys(ID2D1DeviceContext2* context, const KeyboardFrame& frame, TimeUtil::TimePoint now) {
  const auto& faces = *frame.mFaces;
  const auto& keys = faces.mGeometry->mKeys;
  const float inset = (KeyGap / 2) * U;
  const float radius = 0.12f * U;
  for (std::size_t i = 0; i < keys.size() && i < faces.mKeys.size(); ++i) {
    const auto& key = keys[i];
    const auto& face = faces.mKeys[i];
    const int index = static_cast<int>(i);

    uint32_t colour = key.mRole == KeyRole::Character || key.mRole == KeyRole::Numpad ? KeyColour : ActionColour;
    if (face.mFlags & KeyFace::Locked) {
      colour = LockedColour;
    } else if (face.mFlags & KeyFace::Latched) {
      colour = LatchedColour;
    }
    for (std::size_t hand = 0; hand < 2; ++hand) {
      if (frame.mHoverKey[hand] == index) {
        colour = Mix(colour, HoverColour, static_cast<float>(frame.mHoverLevel[hand]) / KeyboardFrame::HoverLevels);
      }
    }
    if (std::ranges::find(frame.mHeld, index) != frame.mHeld.end()) {
      colour = HeldColour;
    }
    for (std::size_t f = 0; f < KeyboardFrame::MaxFlashes; ++f) {
      if (frame.mFlashKey[f] == index && now < frame.mFlashUntil[f]) {
        colour = FlashColour;// shows at once, before the app answers (doc 07)
      }
    }

    const auto rect = Pixels(key.mRect, inset);
    Fill(context, {rect, radius, radius}, colour);
    if (key.mLower) {
      // ISO Enter: its lower part joins the upper one
      auto lower = Pixels(*key.mLower, inset);
      lower.top = rect.top + radius;
      Fill(context, {lower, radius, radius}, colour);
    }

    // The legend: what a key types, big; names of keys ("Enter", "F1"),
    // smaller; a single symbol (the arrows), big
    const bool typing = key.mRole == KeyRole::Character || key.mRole == KeyRole::Numpad;
    const bool big = typing ? face.mText.size() <= 2 : face.mText.size() == 1;
    auto* format = big ? mLegend.get() : mLabel.get();
    const uint32_t textColour = (face.mFlags & KeyFace::Dead) ? DeadText : TextColour;
    if (face.mSmall.empty()) {
      Text(context, face.mText, format, rect, textColour);
    } else if (key.mRole == KeyRole::Numpad) {
      // The digit, and under it what the key does with Num Lock off
      const float split = rect.top + ((rect.bottom - rect.top) * 0.62f);
      Text(context, face.mText, format, {rect.left, rect.top, rect.right, split + (0.04f * U)}, textColour);
      Text(context, face.mSmall, mSmall.get(), {rect.left + (0.02f * U), split - (0.04f * U), rect.right - (0.02f * U),
        rect.bottom - (0.02f * U)}, DimText);
    } else {
      // The AltGr character in the bottom right corner
      Text(context, face.mText, format, {rect.left, rect.top, rect.right, rect.bottom - (0.12f * U)}, textColour);
      Text(context, face.mSmall, mSmall.get(), {rect.left + ((rect.right - rect.left) * 0.5f), rect.bottom - (0.36f * U),
        rect.right - (0.04f * U), rect.bottom - (0.04f * U)}, DimText);
    }

    // Lock lights, and a bar under a locked modifier
    if (key.mRole == KeyRole::CapsLock || key.mRole == KeyRole::NumLock) {
      const bool on = (face.mFlags & KeyFace::LockOn) != 0;
      mBrush->SetColor(Rgb(on ? LightColour : 0x4A4E57));
      mBrush->SetOpacity(mAlpha);
      context->FillEllipse({{rect.right - (0.16f * U), rect.top + (0.16f * U)}, 0.06f * U, 0.06f * U}, mBrush.get());
    }
    if (face.mFlags & KeyFace::Locked) {
      Fill(context, {{rect.left + (0.2f * U), rect.bottom - (0.12f * U), rect.right - (0.2f * U), rect.bottom - (0.07f * U)},
        0.02f * U, 0.02f * U}, TextColour);
    }
  }
}

}// namespace OpenDesktopVR::VirtualKeyboard
