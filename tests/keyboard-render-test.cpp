// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Draws the keyboard panel offscreen (M5b) and saves PNGs, for looking at:
// the US layout plain, and the German one (ISO) plain, with Shift latched,
// with AltGr locked and with a dead key waiting. Checks that each draw
// covers the panel (opaque by default, K9) and that a key press shows.
// Needs a GPU, or WARP; not run by CTest.
//
//   keyboard-render-test <output folder>

#include "check.hpp"
#include "save-png.hpp"

#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/KeyboardModel.hpp>
#include <OpenDesktopVR/KeyboardSource.hpp>
#include <OpenDesktopVR/VirtualKeyboard.hpp>

#include <filesystem>
#include <format>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::VirtualKeyboard;
using Testing::Check;

namespace {

std::shared_ptr<const KeyboardFaces> Faces(const Keyboard& keyboard) {
  auto faces = std::make_shared<KeyboardFaces>();
  faces->mGeometry = keyboard.GetGeometry();
  for (std::size_t i = 0; i < faces->mGeometry->mKeys.size(); ++i) {
    const auto legend = keyboard.GetLegend(static_cast<int>(i));
    KeyFace face {legend.mText, legend.mSmall, static_cast<uint8_t>(legend.mDead ? KeyFace::Dead : 0)};
    const auto role = faces->mGeometry->mKeys[i].mRole;
    const auto modifier = [&](Modifier m) -> uint8_t {
      const auto state = keyboard.GetModifier(m);
      return state == ModifierState::Locked ? KeyFace::Locked : state == ModifierState::Latched ? KeyFace::Latched : 0;
    };
    if (role == KeyRole::Shift) {
      face.mFlags |= modifier(Modifier::Shift);
    } else if (role == KeyRole::AltGr) {
      face.mFlags |= modifier(keyboard.HasAltGr() ? Modifier::AltGr : Modifier::Alt);
    } else if (role == KeyRole::NumLock && keyboard.NumLock()) {
      face.mFlags |= KeyFace::LockOn;
    }
    faces->mKeys.push_back(face);
  }
  faces->mLayoutName = keyboard.ActiveLayout()->GetInfo().mDisplayName;
  if (const auto pending = keyboard.PendingDeadKey()) {
    faces->mPendingDeadKey = keyboard.ActiveLayout()->GetSpacingDeadChar(*pending);
  }
  return faces;
}

}// namespace

int main(int argc, char** argv) {
  const std::filesystem::path folder = argc > 1 ? argv[1] : "keyboard-render";
  std::filesystem::create_directories(folder);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  const auto gpu = Render::GpuDevice::Create({.mAllowWarp = true});
  if (!Check(gpu != nullptr, "a GPU (or WARP)")) {
    return Testing::Finish();
  }
  auto model = std::make_shared<KeyboardModel>();
  KeyboardSource source {*gpu, model, [] {}};
  model->SetShown(true);
  model->SetTarget(L"Discord");

  const auto draw = [&](const char* name, const Keyboard& keyboard, int flash) {
    model->SetFaces(Faces(keyboard));
    if (flash >= 0) {
      model->Flash(flash);
    }
    const auto size = source.Size();
    const D2D1_BITMAP_PROPERTIES1 target {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
      D2D1_BITMAP_OPTIONS_TARGET};
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    gpu->D2D()->CreateBitmap({size.mWidth, size.mHeight}, nullptr, 0, target, bitmap.put());
    const D2D1_BITMAP_PROPERTIES1 readable {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
      D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW};
    winrt::com_ptr<ID2D1Bitmap1> copy;
    gpu->D2D()->CreateBitmap({size.mWidth, size.mHeight}, nullptr, 0, readable, copy.put());
    // Fully faded in: a few draws
    for (int i = 0; i < 6; ++i) {
      gpu->D2D()->SetTarget(bitmap.get());
      gpu->D2D()->BeginDraw();
      source.Render(gpu->D2D(), size, TimeUtil::Now());
      gpu->D2D()->EndDraw();
    }
    gpu->D2D()->SetTarget(nullptr);
    copy->CopyFromBitmap(nullptr, bitmap.get(), nullptr);
    D2D1_MAPPED_RECT mapped {};
    copy->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    std::vector<uint8_t> bgra(static_cast<std::size_t>(size.mWidth) * size.mHeight * 4);
    int opaque = 0;
    for (uint32_t y = 0; y < size.mHeight; ++y) {
      const auto* row = mapped.bits + (static_cast<std::size_t>(y) * mapped.pitch);
      std::memcpy(bgra.data() + (static_cast<std::size_t>(y) * size.mWidth * 4), row, size.mWidth * 4);
      for (uint32_t x = 0; x < size.mWidth; ++x) {
        opaque += row[(x * 4) + 3] == 255 ? 1 : 0;
      }
    }
    copy->Unmap();
    const double share = static_cast<double>(opaque) / (static_cast<double>(size.mWidth) * size.mHeight);
    Check(share > 0.97, std::format("{}: opaque ({:.1f}% of it; the corners are round)", name, share * 100));
    TestUtilities::SavePNG(folder / std::format("{}.png", name), size.mWidth, size.mHeight, bgra);
  };

  const auto us = KeyboardLayout::Layout::Load("00000409");
  const auto german = KeyboardLayout::Layout::Load("00000407");
  if (!Check(us && german, "the US and German layouts load")) {
    return Testing::Finish();
  }
  Keyboard plain {{us}};
  draw("us", plain, FindKey(*plain.GetGeometry(), 0x23));

  Keyboard keyboard {{german}};
  const auto& geometry = *keyboard.GetGeometry();
  draw("german", keyboard, -1);
  keyboard.Press(FindKey(geometry, 0x2A), 1.0);
  draw("german-shift", keyboard, FindKey(geometry, 0x2A));
  keyboard.Press(FindKey(geometry, 0x2A), 1.1);// locks
  keyboard.Press(FindKey(geometry, 0x2A), 2.0);// clears
  keyboard.Press(FindRole(geometry, KeyRole::AltGr), 3.0);
  keyboard.Press(FindRole(geometry, KeyRole::AltGr), 3.1);
  draw("german-altgr-locked", keyboard, -1);
  keyboard.Press(FindRole(geometry, KeyRole::AltGr), 4.0);
  keyboard.Press(FindKey(geometry, 0x0D), 5.0);
  draw("german-dead-key", keyboard, FindKey(geometry, 0x0D));
  std::printf("PNGs in %s\n", std::filesystem::absolute(folder).string().c_str());
  return Testing::Finish();
}
