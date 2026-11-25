// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The host's panel model, without a GPU:
// - trims in pixels (CT-12): what's left of a window, and how trims that
//   would leave too little shrink;
// - which way a panel faces: toward the viewer (turned and tilted), or the
//   turn and tilt set by hand (PN-2; test round 5 asked for tilt);
// - the settings file: tilt and pixel trims round-trip, the old fraction
//   trims are dropped, bad values are refused (ST-5, TH-9);
// - ring textures only grow, in steps (RN-7);
// - showing and hiding (PN-6): the global toggle's three cases, one panel
//   while all are hidden, hides of removed panels;
// - a panel's look (PN-4, PN-5): stored, odd values brought into range, the
//   colour multipliers; its keybinds and the global ones, stored;
// - the keyboard (M5b): its settings, where its panel goes, the renderer
//   getting it first in a full list (VK-5), and the caret mapped into the
//   target panel's pixels for the preview strip (K13).

#include "check.hpp"

#include <OpenDesktopVR/DocumentFormats.hpp>
#include <OpenDesktopVR/HostPanel.hpp>
#include <OpenDesktopVR/KeyboardConfig.hpp>
#include <OpenDesktopVR/KeyboardService.hpp>
#include <OpenDesktopVR/PanelConfig.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/PanelRing.hpp>
#include <OpenDesktopVR/Visibility.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Host;
using Testing::Check;
using Testing::Scenario;

namespace {

// The panel's normal (+Z turned by its orientation)
std::array<float, 3> Normal(const PanelDefinition& panel) {
  float q[4];
  PanelOrientation(panel, q);
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  // Rotating (0, 0, 1): the third column of the rotation matrix
  return {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
}

bool Near(float a, float b, float tolerance = 1e-4f) {
  return std::abs(a - b) <= tolerance;
}

// The direction from the panel to the origin
std::array<float, 3> TowardOrigin(const PanelDefinition& panel) {
  const float length = std::sqrt(panel.mPosition[0] * panel.mPosition[0] + panel.mPosition[1] * panel.mPosition[1]
    + panel.mPosition[2] * panel.mPosition[2]);
  return {-panel.mPosition[0] / length, -panel.mPosition[1] / length, -panel.mPosition[2] / length};
}

PanelDefinition WindowPanel() {
  PanelDefinition panel;
  panel.mID = 0x1234;
  panel.mName = L"Notepad";
  panel.mKind = PanelKind::Window;
  panel.mWindow = {L"C:\\Windows\\notepad.exe", L"Notepad", L"Untitled - Notepad", WindowMatch::TitleMode::Prefer};
  return panel;
}

}// namespace

int main() {
  Scenario("Pixel trims leave the rest of the window (CT-12)", [] {
    const Render::Crop crop {10, 40, 20, 0};
    const auto kept = crop.Apply({1920, 1080});
    Check(kept == Render::PixelRect {10, 40, 1890, 1040}, std::format("1920x1080 trimmed: {},{} {}x{}", kept.mLeft, kept.mTop, kept.mWidth, kept.mHeight));
    Check(Render::Crop {}.Apply({800, 600}) == Render::PixelRect {0, 0, 800, 600}, "no trim: all of it");
    Check(Render::Crop {}.IsEmpty() && !crop.IsEmpty(), "IsEmpty");
  });

  Scenario("The same trim keeps the same pixels when the window is resized (test round 5)", [] {
    const Render::Crop titleBar {0, 72, 0, 0};
    const auto smaller = titleBar.Apply({800, 600});
    const auto larger = titleBar.Apply({1600, 1200});
    Check(smaller.mTop == 72 && larger.mTop == 72, "72 px off the top at either size");
    Check(smaller.mHeight == 528 && larger.mHeight == 1128, "the rest shown");
  });

  Scenario("Trims that would leave too little shrink in proportion; something always shows", [] {
    const Render::Crop crop {300, 0, 100, 0};
    const auto kept = crop.Apply({200, 100});
    Check(kept.mWidth == Render::Crop::MinimumLeft, std::format("{} px left (at least {})", kept.mWidth, Render::Crop::MinimumLeft));
    Check(kept.mLeft == 138, std::format("the left trim keeps its share: 300/400 of 184 = {}", kept.mLeft));
    Check(kept.mLeft + kept.mWidth <= 200, "within the window");
    const auto tiny = Render::Crop {5, 5, 5, 5}.Apply({8, 4});
    Check(tiny.mWidth == 8 && tiny.mHeight == 4 && tiny.mLeft == 0 && tiny.mTop == 0, "a window smaller than the minimum: all of it");
    const auto empty = crop.Apply({0, 0});
    Check(empty.mWidth == 0 && empty.mHeight == 0, "no window: nothing, and no division by zero");
    const auto huge = Render::Crop {4'000'000'000u, 0, 4'000'000'000u, 0}.Apply({1000, 10});
    Check(huge.mWidth == Render::Crop::MinimumLeft && huge.mLeft + huge.mWidth <= 1000, "absurd trims: no overflow");
  });

  Scenario("A panel facing me points at my head from anywhere, tilted as well as turned", [] {
    for (const auto& position: {std::array {0.0f, 0.0f, -0.6f}, std::array {-0.4f, 0.0f, -0.6f}, std::array {0.4f, 0.1f, -0.9f},
           std::array {0.0f, -0.45f, -0.5f}, std::array {0.7f, 0.5f, 0.2f}}) {
      PanelDefinition panel;
      std::copy(position.begin(), position.end(), panel.mPosition);
      panel.mFaceOrigin = true;
      const auto normal = Normal(panel);
      const auto toward = TowardOrigin(panel);
      Check(Near(normal[0], toward[0], 1e-3f) && Near(normal[1], toward[1], 1e-3f) && Near(normal[2], toward[2], 1e-3f),
        std::format("at ({}, {}, {}): normal ({:.3f}, {:.3f}, {:.3f}), toward the head ({:.3f}, {:.3f}, {:.3f})", position[0],
          position[1], position[2], normal[0], normal[1], normal[2], toward[0], toward[1], toward[2]));
    }
  });

  Scenario("Turn and tilt by hand (test round 5)", [] {
    PanelDefinition panel;
    panel.mFaceOrigin = false;
    auto normal = Normal(panel);
    Check(Near(normal[0], 0) && Near(normal[1], 0) && Near(normal[2], 1), "0, 0: faces straight back at the viewer (+Z)");
    panel.mYawDegrees = 90;
    normal = Normal(panel);
    Check(Near(normal[0], 1) && Near(normal[1], 0) && Near(normal[2], 0), "turn 90: faces +X (turned to its left)");
    panel.mYawDegrees = 0;
    panel.mPitchDegrees = 30;
    normal = Normal(panel);
    Check(Near(normal[1], 0.5f) && Near(normal[2], std::sqrt(3.0f) / 2), "tilt 30: faces 30 degrees up");
    panel.mYawDegrees = 90;
    normal = Normal(panel);
    Check(Near(normal[0], std::sqrt(3.0f) / 2) && Near(normal[1], 0.5f) && Near(normal[2], 0),
      "turn 90 then tilt 30: still 30 degrees up (tilted about its own axis)");
    float q[4];
    PanelOrientation(panel, q);
    Check(Near(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1), "a unit quaternion");
  });

  Scenario("Unticking 'Face me' starts from the angles it faced", [] {
    PanelDefinition panel;
    panel.mPosition[0] = 0.4f;
    panel.mPosition[1] = -0.3f;
    panel.mPosition[2] = -0.7f;
    const auto before = Normal(panel);
    const auto facing = FacingOrigin(panel.mPosition);
    panel.mFaceOrigin = false;
    panel.mYawDegrees = facing.mYawDegrees;
    panel.mPitchDegrees = facing.mPitchDegrees;
    const auto after = Normal(panel);
    Check(Near(before[0], after[0]) && Near(before[1], after[1]) && Near(before[2], after[2]), "no jump");
    Check(facing.mPitchDegrees > 0, std::format("below the eyes it tilts up ({:.1f} degrees)", facing.mPitchDegrees));
  });

  Scenario("Tilt and pixel trims are saved and read back", [] {
    auto panel = WindowPanel();
    panel.mFaceOrigin = false;
    panel.mYawDegrees = -12.5f;
    panel.mPitchDegrees = 20;
    panel.mCrop = {3, 72, 4, 9};
    const auto json = PanelConfig::ToJson(panel);
    Check(json.contains("cropPixels") && !json.contains("crop"), "trims stored as \"cropPixels\"");
    const auto back = PanelConfig::FromJson(json);
    Check(back.has_value(), "read back");
    if (back) {
      Check(back->mPitchDegrees == 20 && back->mYawDegrees == -12.5f && !back->mFaceOrigin, "the angles");
      Check(back->mCrop == panel.mCrop, "the trims");
      Check(back->SameContent(panel), "the same window rule");
    }
  });

  Scenario("Old fraction trims are dropped, not misread as pixels", [] {
    auto json = PanelConfig::ToJson(WindowPanel());
    json.erase("cropPixels");
    json["crop"] = JsonUtil::Json::array({0.0, 0.12, 0.0, 0.0});
    const auto panel = PanelConfig::FromJson(json);
    Check(panel.has_value() && panel->mCrop.IsEmpty(), "read, untrimmed");
    if (panel) {
      const auto saved = PanelConfig::ToJson(*panel);
      Check(!saved.contains("crop"), "and not written back");
    }
  });

  Scenario("Bad values from a hand-edited file (TH-9)", [] {
    auto json = PanelConfig::ToJson(WindowPanel());
    json["pitchDegrees"] = 135;
    Check(!PanelConfig::FromJson(json).has_value(), "a tilt past 90 degrees is refused");
    json["pitchDegrees"] = 10;
    json["cropPixels"] = JsonUtil::Json::array({-5, "x", 1e12, 40});
    const auto panel = PanelConfig::FromJson(json);
    Check(panel.has_value(), "odd trims don't lose the panel");
    if (panel) {
      Check(panel->mCrop.mLeft == 0 && panel->mCrop.mTop == 0, "negative and non-numbers: 0");
      Check(panel->mCrop.mRight == Render::Crop::MaximumEdge && panel->mCrop.mBottom == 40, "huge: capped");
    }
    json.erase("pitchDegrees");
    const auto old = PanelConfig::FromJson(json);
    Check(old.has_value() && old->mPitchDegrees == 0, "a file without tilt: 0");
  });

  Scenario("Ring textures only grow, in 256-pixel steps (RN-7)", [] {
    using Render::PanelRing;
    const auto first = PanelRing::CapacityFor({960, 600}, {});
    Check(first == Render::PixelSize {1024, 768}, std::format("960x600 first: {}x{}", first.mWidth, first.mHeight));
    Check(PanelRing::CapacityFor({500, 300}, first) == first, "smaller content: the same textures");
    const auto wider = PanelRing::CapacityFor({1100, 300}, first);
    Check(wider == Render::PixelSize {1280, 768}, std::format("wider: grows one step ({}x{}), keeps its height", wider.mWidth, wider.mHeight));
    Check(PanelRing::CapacityFor({16000, 100}, {}).mWidth == 16128, "16000 wide: 16128, within Direct3D's 16384");
    Check(PanelRing::CapacityFor({256, 256}, {}) == Render::PixelSize {256, 256}, "an exact step stays");
  });

  Scenario("Show or hide all (PN-6): any showing hides all; none showing shows all; none enabled enables all", [] {
    std::vector<PanelDefinition> panels(3);
    for (int i = 0; i < 3; ++i) {
      panels[i].mID = static_cast<uint64_t>(i + 1);
    }
    panels[2].mEnabled = false;
    Visibility::Hidden hidden {2};
    Check(Visibility::AnyShowing(panels, hidden), "panel 1 is showing");
    auto result = Visibility::ToggleAll(panels, hidden);
    Check(result.mHidden == Visibility::Hidden {1, 2} && !result.mEnableAll, "all enabled panels hidden");
    Check(!Visibility::AnyShowing(panels, result.mHidden), "none showing");
    result = Visibility::ToggleAll(panels, result.mHidden);
    Check(result.mHidden.empty() && !result.mEnableAll, "toggled again: every hide cleared");
    Check(Visibility::IsShowing(panels[0], result.mHidden) && Visibility::IsShowing(panels[1], result.mHidden), "both enabled panels show");
    Check(!Visibility::IsShowing(panels[2], result.mHidden), "the disabled one stays off");
    for (auto& panel: panels) {
      panel.mEnabled = false;
    }
    result = Visibility::ToggleAll(panels, {});
    Check(result.mEnableAll, "none enabled: the toggle enables them all, so it never seems to do nothing");
  });

  Scenario("One panel's toggle while all are hidden shows just that one; removed panels' hides go", [] {
    std::vector<PanelDefinition> panels(2);
    panels[0].mID = 1;
    panels[1].mID = 2;
    auto hidden = Visibility::ToggleAll(panels, {}).mHidden;
    hidden = Visibility::TogglePanel(panels, hidden, 2);
    Check(!Visibility::IsShowing(panels[0], hidden) && Visibility::IsShowing(panels[1], hidden), "only panel 2 shows");
    hidden = Visibility::TogglePanel(panels, hidden, 2);
    Check(!Visibility::AnyShowing(panels, hidden), "toggled again: hidden");
    panels[0].mEnabled = false;
    Check(Visibility::TogglePanel(panels, hidden, 1) == hidden, "a disabled panel's toggle changes nothing");
    panels.pop_back();
    Check(!Visibility::Prune(panels, hidden).contains(2), "a removed panel's hide is dropped");
  });

  Scenario("A panel's look is stored; odd values are brought into range (PN-4, PN-5)", [] {
    auto panel = WindowPanel();
    panel.mOpacity = 0.6f;
    panel.mFocusedOpacity = 1.0f;
    panel.mBrightness = 0.3f;
    panel.mTintEnabled = true;
    panel.mTintColor = 0xFF2010;
    auto json = PanelConfig::ToJson(panel);
    Check(json.value("tintColor", "") == "#FF2010", "the tint is written as #RRGGBB");
    auto back = PanelConfig::FromJson(json);
    Check(back && back->SameLook(panel), "read back the same");
    json["opacity"] = 0.0;
    json["brightness"] = 7.5;
    json["tintColor"] = "red";
    back = PanelConfig::FromJson(json);
    Check(back && back->mOpacity == PanelDefinition::MinimumLevel, "opacity 0: 10% (never invisible)");
    Check(back && back->mBrightness == 1.0f, "brightness 7.5: 100%");
    Check(back && back->mTintColor == PanelDefinition::DefaultTintColor, "a bad colour: the default");
    json.erase("opacity");
    json.erase("brightness");
    back = PanelConfig::FromJson(json);
    Check(back && back->mOpacity == 1.0f && back->mBrightness == 1.0f, "a file without them: 100%");
  });

  Scenario("The look as colour multipliers: brightness times tint on colour, opacity on alpha", [] {
    PanelDefinition panel;
    Check(panel.IsPlain(false) && panel.IsPlain(true), "the defaults are plain: drawn as they are");
    panel.mBrightness = 0.5f;
    panel.mTintEnabled = true;
    panel.mTintColor = 0xFF8000;
    panel.mOpacity = 0.4f;
    panel.mFocusedOpacity = 0.9f;
    const auto normal = LookMultipliers(panel, false);
    Check(Near(normal[0], 0.5f) && Near(normal[1], 0.5f * 128 / 255, 1e-3f) && Near(normal[2], 0) && Near(normal[3], 0.4f),
      std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", normal[0], normal[1], normal[2], normal[3]));
    Check(Near(LookMultipliers(panel, true)[3], 0.9f), "looked at: its focused opacity");
    panel.mTintEnabled = false;
    panel.mBrightness = 1;
    panel.mOpacity = 1;
    Check(panel.IsPlain(false) && !panel.IsPlain(true), "plain until looked at, when the focused opacity applies");
  });

  Scenario("Keep updating when covered: on by default, stored when off (CT-3, Q10)", [] {
    auto panel = WindowPanel();
    Check(panel.mKeepUpdatingWhenCovered, "a new window panel: on");
    auto json = PanelConfig::ToJson(panel);
    json.erase("keepUpdatingWhenCovered");
    auto back = PanelConfig::FromJson(json);
    Check(back && back->mKeepUpdatingWhenCovered, "a file from before M3d: on");
    panel.mKeepUpdatingWhenCovered = false;
    back = PanelConfig::FromJson(PanelConfig::ToJson(panel));
    Check(back && !back->mKeepUpdatingWhenCovered, "turned off: read back off");
    Check(back && back->SameContent(panel), "not part of the content: changing it doesn't restart the capture");
  });

  Scenario("Document panels in settings.json (M8a): the path, the page counted from 1, nonsense refused", [] {
    auto panel = NewDocumentPanel(L"C:\\Flight\\Checklist.pdf", 4);
    Check(panel.mKind == PanelKind::Document && panel.mName == L"Checklist" && panel.mPage == 0 && panel.mID != 0,
      "a new one: named after the file, on its first page");
    Check(DocumentPanelName(L"C:\\Flight\\Charts") == L"Charts" && DocumentPanelName(L"D:\\") == L"D:\\",
      "a folder's name; a drive's root: its path");
    panel.mPage = 2;
    auto json = PanelConfig::ToJson(panel);
    Check(json["kind"] == "document" && json["document"]["path"] == "C:\\Flight\\Checklist.pdf" && json["document"]["page"] == 3
        && !json.contains("window") && !json.contains("animated"),
      "stored: kind, path, and page 3 (counted from 1), nothing of a window's");
    auto back = PanelConfig::FromJson(json);
    Check(back && back->mKind == PanelKind::Document && back->mDocumentPath == panel.mDocumentPath && back->mPage == 2
        && back->SameContent(panel),
      "read back the same");
    auto other = panel;
    other.mPage = 5;
    other.mWidthMeters = 0.5f;
    Check(other.SameContent(panel), "another page or size is the same content: the source stays");
    other.mDocumentPath = L"C:\\Flight\\Other.pdf";
    Check(!other.SameContent(panel), "another file isn't");
    Check(PageViewFor(panel).mPage == 2 && PageViewFor(panel).mWidth == Documents::PageWidthFor(panel.mWidthMeters),
      "its page view: the page, at the width from its size in VR");
    json["document"]["page"] = 0;
    Check(PanelConfig::FromJson(json) && PanelConfig::FromJson(json)->mPage == 0, "page 0 (nonsense): the first");
    json["document"]["page"] = -4;
    Check(PanelConfig::FromJson(json) && PanelConfig::FromJson(json)->mPage == 0, "a negative page: the first");
    json["document"]["page"] = "seven";
    Check(PanelConfig::FromJson(json) && PanelConfig::FromJson(json)->mPage == 0, "a page that isn't a number: the first");
    json["document"]["page"] = 1e30;
    Check(PanelConfig::FromJson(json) && PanelConfig::FromJson(json)->mPage == 0, "a page beyond any count: the first");
    json["document"].erase("path");
    Check(!PanelConfig::FromJson(json), "no path: skipped (ST-4)");
    json["document"]["path"] = std::string(200'000, 'x');
    Check(!PanelConfig::FromJson(json), "a path longer than Windows allows: skipped");
    json["document"] = "C:\\file.pdf";
    Check(!PanelConfig::FromJson(json), "\"document\" not an object: skipped");
    auto future = PanelConfig::ToJson(panel);
    future["document"]["outline"] = true;
    future["zoom"] = 2;
    const auto kept = PanelConfig::ToJson(*PanelConfig::FromJson(future));
    Check(kept.contains("zoom") && kept["document"].value("outline", false) && kept["document"]["page"] == 3,
      "a newer version's fields are kept, at the top and in \"document\" (ST-5)");
    auto asWindow = *PanelConfig::FromJson(future);
    asWindow.mKind = PanelKind::Window;
    Check(!PanelConfig::ToJson(asWindow).contains("document"), "changed to a window: the document goes");
  });

  Scenario("Keybinds are stored with the panel and in the global section (UI-2, UI-3)", [] {
    auto panel = WindowPanel();
    const auto f13 = Keybinds::Binding::From({Keybinds::Button {Keybinds::DeviceKind::Keyboard, {}, VK_F13}});
    panel.KeybindsFor(Keybinds::ActionKind::ToggleTint)[1] = f13;
    const auto back = PanelConfig::FromJson(PanelConfig::ToJson(panel));
    Check(back && back->KeybindsFor(Keybinds::ActionKind::ToggleTint)[1] == f13, "a panel's tint keybind round-trips");
    Check(back && back->KeybindsFor(Keybinds::ActionKind::TogglePanel)[0].IsEmpty(), "the others stay empty");
    JsonUtil::Json settings {{"keybinds", {{"futureAction", 1}}}};
    PanelConfig::WriteGlobalKeybinds(settings, Keybinds::ActionKind::ToggleAllPanels, {f13, {}});
    PanelConfig::WriteGlobalKeybinds(settings, Keybinds::ActionKind::ToggleKeyboard, Keybinds::Slots {Keybinds::Binding {}, f13});
    Check(PanelConfig::ReadGlobalKeybinds(settings, Keybinds::ActionKind::ToggleAllPanels)[0] == f13,
      "the global keybind round-trips");
    Check(PanelConfig::ReadGlobalKeybinds(settings, Keybinds::ActionKind::ToggleKeyboard)[1] == f13
        && PanelConfig::ReadGlobalKeybinds(settings, Keybinds::ActionKind::ToggleAllPanels)[1].IsEmpty(),
      "the keyboard's keybind round-trips beside it (K5)");
    Check(settings["keybinds"].contains("futureAction"), "a newer version's keybinds are kept (ST-5)");
    auto document = NewDocumentPanel(L"C:\\Manual.pdf", 0);
    document.KeybindsFor(Keybinds::ActionKind::NextPage)[0] = f13;
    const auto json = PanelConfig::ToJson(document);
    Check(json["keybinds"].contains("nextPage") && json["keybinds"].contains("previousPage"), "page keybinds stored by name (M8a)");
    const auto read = PanelConfig::FromJson(json);
    Check(read && read->KeybindsFor(Keybinds::ActionKind::NextPage)[0] == f13
        && read->KeybindsFor(Keybinds::ActionKind::PreviousPage)[0].IsEmpty(),
      "the next-page keybind round-trips");
    Check(Keybinds::IsDocumentAction(Keybinds::ActionKind::NextPage) && !Keybinds::IsDocumentAction(Keybinds::ActionKind::ToggleTint)
        && !Keybinds::IsGlobal(Keybinds::ActionKind::PreviousPage),
      "page actions belong to a document panel");
  });

  Scenario("The keyboard's settings: defaults, round trip, odd values (ST-4, ST-5)", [] {
    JsonUtil::Json settings = JsonUtil::Json::object();
    const auto defaults = KeyboardConfig::Read(settings);
    Check(defaults.mEnabled && defaults.mPrimaryLayout.empty() && defaults.mFunctionRow && !defaults.mHideAfterEnter,
      "on, \"My Windows layouts\", function row, Enter doesn't hide (K6, K8, K10)");
    Check(defaults.mPlacement == KeyboardSettings::DefaultPlacement() && defaults.mKeyMillimeters == 24,
      "50 cm away, 30 cm below the eyes, tilted 45, 24 mm keys (doc 07, Q7)");
    KeyboardSettings changed;
    changed.mPrimaryLayout = "00000407";
    changed.mSecondaryLayout = "00000409";
    changed.mClickVolume = 0;
    changed.mHideAfterEnter = true;
    changed.mPlacement.mPitchDegrees = 30;
    changed.mPlacement.mPosition[0] = 0.12f;
    changed.mPlacement.mBrightness = 0.5f;
    changed.mPlacement.mTintEnabled = true;
    KeyboardConfig::Write(settings, changed);
    Check(KeyboardConfig::Read(settings) == changed, "a changed keyboard round-trips, its placement and look too (test round 19)");
    settings["keyboard"]["position"] = JsonUtil::Json::array({9, 0, 0.5});
    settings["keyboard"]["keyMillimeters"] = "big";
    settings["keyboard"]["primaryLayout"] = "../../evil";
    const auto odd = KeyboardConfig::Read(settings);
    Check(odd.mPlacement.mPosition[0] == 5.0f && std::abs(odd.mPlacement.mPosition[2] + 0.1f) < 0.0001f
        && odd.mKeyMillimeters == 24 && odd.mPrimaryLayout.empty(),
      "5 m to the side at most, never behind; not a number is the default; not a layout ID is none");

    auto old = JsonUtil::Json::parse(R"({"keyboard": {"distanceCentimeters": 60, "heightCentimeters": 20, "tiltDegrees": 30}})");
    const auto migrated = KeyboardConfig::Read(old);
    Check(migrated.mPlacement == PanelPlacementConfig::FromDistance(60, 20, 0, 30),
      "an older file's distance, height and tilt: the same place, as a panel's settings");
    KeyboardConfig::Write(old, migrated);
    Check(!old["keyboard"].contains("distanceCentimeters") && !old["keyboard"].contains("tiltDegrees")
        && old["keyboard"].contains("position") && KeyboardConfig::Read(old) == migrated,
      "saved as a panel's settings; the old ones go");
  });

  Scenario("The keyboard's panel: below the eyes, at its distance, tilted up, sized by its keys (doc 07, Q7)", [] {
    KeyboardSettings keyboard;
    const auto panel = KeyboardPanel(keyboard);
    const float distance = std::sqrt((panel.mPosition[0] * panel.mPosition[0]) + (panel.mPosition[1] * panel.mPosition[1])
      + (panel.mPosition[2] * panel.mPosition[2]));
    Check(panel.mID == KeyboardPanelID && panel.mKind == PanelKind::Keyboard, "the keyboard's own ID and kind");
    Check(std::abs(distance - 0.5f) < 0.001f && std::abs(panel.mPosition[1] + 0.30f) < 0.001f && panel.mPosition[2] < 0,
      std::format("50 cm from the eyes, 30 cm below them, in front ({:.3f} m)", distance));
    const auto normal = Normal(panel);
    Check(normal[1] > 0.6f && normal[2] > 0.6f, "tilted 45 degrees to face up and back toward you");
    Check(std::abs(panel.mWidthMeters - 0.552f) < 0.001f, std::format("23 keys of 24 mm wide ({:.3f} m)", panel.mWidthMeters));
    Check(!panel.mTouch && !panel.mScroll, "typed on, never pointed at");
    Check(panel.mMaxUpdatesPerSecond == 30, "drawn at most 30 times a second (doc 08)");
  });

  Scenario("The keyboard goes to the renderer first, so a full list never leaves it out (VK-5)", [] {
    std::vector<PanelDefinition> full;
    for (std::size_t i = 0; i < SharedAbi::MaxPanels; ++i) {
      PanelDefinition panel;
      panel.mID = 100 + i;
      full.push_back(panel);
    }
    KeyboardSettings keyboard;
    const auto panels = PanelsForRenderer(full, &keyboard);
    Check(!panels.empty() && panels.front().mID == KeyboardPanelID, "the keyboard is first");
    Check(panels.size() == full.size() + 1, "then every enabled panel");
    Check(PanelsForRenderer(full, nullptr).size() == full.size(), "turned off: not there at all");
  });

  Scenario("The caret in the target panel's pixels, for the preview strip (K13)", [] {
    // A window whose client area is 1000 x 800 of its own units, captured
    // at 2000 x 1600 (200% scaling), the panel showing from (100, 50)
    const Pointer::Target target {nullptr, {100, 50, 1900, 1550}, {2000, 1600}};
    const auto caret = CaretInContent(target, {500, 400, 502, 420}, 1000, 800);
    Check(caret.left == 900 && caret.top == 750 && caret.right == 904 && caret.bottom == 790,
      std::format("mapped like a tap the other way ({}, {}, {}, {})", caret.left, caret.top, caret.right, caret.bottom));
    const auto point = Pointer::ClientPoint(target, 900.0f / 1800.0f, 750.0f / 1500.0f, 1000, 800);
    Check(point.x == 500 && point.y == 400, "and a tap there lands back on it");
  });

  Scenario("Size as a box (PN-14): the width, unless the height would pass its limit", [] {
    PanelDefinition panel;
    panel.mID = 0xE1;// a real panel's: one without an ID isn't a panel
    panel.mName = L"Tall";
    panel.mWidthMeters = 0.4f;
    auto size = PanelSizeFor(panel, 1000, 2000);
    Check(std::abs(size.mWidth - 0.4f) < 1e-5f && std::abs(size.mHeight - 0.8f) < 1e-5f, "no limit: the width, the height from the shape");
    panel.mMaxHeightMeters = 0.5f;
    size = PanelSizeFor(panel, 1000, 2000);
    Check(std::abs(size.mWidth - 0.25f) < 1e-5f && std::abs(size.mHeight - 0.5f) < 1e-5f,
      std::format("a tall window fits the box, keeping its shape ({:.3f} x {:.3f})", size.mWidth, size.mHeight));
    size = PanelSizeFor(panel, 2000, 1000);
    Check(std::abs(size.mWidth - 0.4f) < 1e-5f && std::abs(size.mHeight - 0.2f) < 1e-5f, "a wide one is under the limit: unchanged");
    Check(PanelSizeFor(panel, 0, 100).mWidth == 0, "no content yet: no size");
    const auto back = PanelConfig::FromJson(PanelConfig::ToJson(panel));
    Check(back && std::abs(back->mMaxHeightMeters - 0.5f) < 1e-5f, "the limit is saved and read back");
    auto json = PanelConfig::ToJson(panel);
    json["maxHeightMeters"] = -3;
    Check(!PanelConfig::FromJson(json), "a negative limit is refused (TH-9)");
  });

  return Testing::Finish();
}
