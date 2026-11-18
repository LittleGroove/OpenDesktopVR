// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Draws the quick menu offscreen (M6d) and saves PNGs, for looking at: the
// closed button (plain and hovered), the start page with a hover and a
// press, a long list's second rows, a panel's page, the windows, the
// profiles and the settings. Checks each draw is the layout's size, covers
// it (the corners are round; the button is a circle), and that hover and
// press change pixels.
// Needs a GPU, or WARP; not run by CTest.
//
//   quickmenu-render-test <output folder>

#include "check.hpp"
#include "save-png.hpp"

#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/QuickMenuSource.hpp>

#include <cstring>
#include <filesystem>
#include <format>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::QuickMenu;
using Testing::Check;

namespace {

MenuData Data() {
  MenuData data;
  const wchar_t* names[] = {L"Video", L"Spotify", L"EDCoPilot", L"COVAS:NEXT", L"Discord", L"Notepad",
    L"A panel whose name is far too long to fit"};
  uint64_t id = 100;
  for (const auto* name: names) {
    PanelInfo panel;
    panel.mID = id++;
    panel.mName = name;
    panel.mWindow = true;
    panel.mShowing = panel.mID != 102;
    panel.mOpacity = 0.8f;
    panel.mBrightness = 1.0f;
    panel.mWidthMeters = 0.42f;
    panel.mDistanceMeters = 0.65f;
    data.mPanels.push_back(panel);
  }
  data.mProfiles = {{"a", L"Default", false}, {"b", L"Elite Dangerous", true}, {"c", L"Desktop", false}};
  data.mProfileName = L"Elite Dangerous";
  data.mAnyShowing = true;
  data.mWindows = std::vector<WindowEntry> {{L"Spotify Premium", L"Spotify"}, {L"#general | Discord", L"Discord"},
    {L"YouTube - a video with a long title that goes on and on - Mozilla Firefox", L"firefox"}, {L"Untitled - Notepad", L"notepad"},
    {L"EDCoPilot", L"EDCoPilot"}, {L"Calculator", L"CalculatorApp"}};
  return data;
}

int Find(const Layout& layout, CommandKind kind, uint64_t arg = 0) {
  for (std::size_t i = 0; i < layout.mItems.size(); ++i) {
    if (layout.mItems[i].mCommand.mKind == kind && layout.mItems[i].mCommand.mArg == arg) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}// namespace

int main(int argc, char** argv) {
  const std::filesystem::path folder = argc > 1 ? argv[1] : "quickmenu-render";
  std::filesystem::create_directories(folder);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  const auto gpu = Render::GpuDevice::Create({.mAllowWarp = true});
  if (!Check(gpu != nullptr, "a GPU (or WARP)")) {
    return Testing::Finish();
  }
  auto model = std::make_shared<Host::QuickMenuModel>();
  Host::QuickMenuSource source {*gpu, model, [] {}};

  // Draws what the model holds; its pixels (BGRA)
  const auto draw = [&](const char* name, double minimumOpaque) {
    const auto size = source.Size();
    const D2D1_BITMAP_PROPERTIES1 target {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
      D2D1_BITMAP_OPTIONS_TARGET};
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    gpu->D2D()->CreateBitmap({size.mWidth, size.mHeight}, nullptr, 0, target, bitmap.put());
    const D2D1_BITMAP_PROPERTIES1 readable {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
      D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW};
    winrt::com_ptr<ID2D1Bitmap1> copy;
    gpu->D2D()->CreateBitmap({size.mWidth, size.mHeight}, nullptr, 0, readable, copy.put());
    gpu->D2D()->SetTarget(bitmap.get());
    gpu->D2D()->BeginDraw();
    source.Render(gpu->D2D(), size, TimeUtil::Now());
    const auto drawn = gpu->D2D()->EndDraw();
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
        opaque += row[(x * 4) + 3] >= 200 ? 1 : 0;// the closed button is drawn at 92%
      }
    }
    copy->Unmap();
    const double share = static_cast<double>(opaque) / (static_cast<double>(size.mWidth) * size.mHeight);
    Check(SUCCEEDED(drawn) && share >= minimumOpaque,
      std::format("{}: {} x {}, drawn, {:.1f}% covered", name, size.mWidth, size.mHeight, share * 100));
    TestUtilities::SavePNG(folder / std::format("{}.png", name), size.mWidth, size.mHeight, bgra);
    return bgra;
  };
  const auto show = [&](const MenuState& state, const MenuData& data) {
    auto layout = std::make_shared<const Layout>(BuildLayout(state, data));
    model->SetLayout(layout, false);
    return layout;
  };

  auto data = Data();
  auto layout = show({}, data);
  const auto closed = draw("closed", 0.6);
  model->SetPointer(layout.get(), 0, -1);
  const auto hovered = draw("closed-hover", 0.6);
  Check(closed != hovered, "the button looks different under the finger");

  layout = show({.mOpen = true}, data);
  const auto home = draw("home", 0.97);
  Check(source.Size().mWidth == Width && source.Size().mHeight == layout->mHeight, "the start page is the layout's size");
  model->SetPointer(layout.get(), Find(*layout, CommandKind::ShowPanelPage, 101), Find(*layout, CommandKind::TogglePanel, 100));
  const auto pointed = draw("home-hover-press", 0.97);
  Check(home != pointed, "hover and press show");
  show({.mOpen = true, .mFirst = 5}, data);
  draw("home-more", 0.97);
  show({.mOpen = true, .mPage = Page::Panel, .mPanel = 100}, data);
  draw("panel", 0.97);
  show({.mOpen = true, .mPage = Page::Windows, .mPanel = 100}, data);
  draw("windows", 0.97);
  auto finding = data;
  finding.mWindows.reset();
  show({.mOpen = true, .mPage = Page::Windows, .mPanel = 100}, finding);
  draw("windows-finding", 0.97);
  show({.mOpen = true, .mPage = Page::Profiles}, data);
  draw("profiles", 0.97);
  data.mButton = false;
  show({.mOpen = true, .mPage = Page::Settings}, data);
  draw("settings", 0.97);
  std::printf("PNGs in %s\n", std::filesystem::absolute(folder).string().c_str());
  return Testing::Finish();
}
