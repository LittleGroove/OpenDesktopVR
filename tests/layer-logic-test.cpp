// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// The layer's pure logic: layer room (XR-9), composition order and limits,
// the hand-tracking extension policy (XR-7), the per-game off list (VR-8),
// the test card, the OpenXR helpers (XR-10), and where the hands fall in
// each eye's image (XR-13).

#include "check.hpp"

#include <OpenDesktopVR/Layer/Composition.hpp>
#include <OpenDesktopVR/Layer/ExtensionPolicy.hpp>
#include <OpenDesktopVR/Layer/HandBounds.hpp>
#include <OpenDesktopVR/Layer/LayerChain.hpp>
#include <OpenDesktopVR/Layer/LayerConfig.hpp>
#include <OpenDesktopVR/Layer/TestPattern.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/XrUtil.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Layer;
using namespace OpenDesktopVR::Testing;

namespace {

// ---- A fake "next layer" for the extension probe -----------------------------
enum class FakeNext { HasHands, NoHands, LookupFails, EnumerateFails };
FakeNext gFake = FakeNext::HasHands;

XRAPI_ATTR XrResult XRAPI_CALL
FakeEnumerate(const char*, uint32_t capacity, uint32_t* count, XrExtensionProperties* properties) {
  if (gFake == FakeNext::EnumerateFails) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
  std::vector<const char*> names {"XR_KHR_D3D11_enable"};
  if (gFake == FakeNext::HasHands) {
    names.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
  }
  *count = static_cast<uint32_t>(names.size());
  if (capacity == 0) {
    return XR_SUCCESS;
  }
  for (std::size_t i = 0; i < names.size(); ++i) {
    strcpy_s(properties[i].extensionName, names[i]);
  }
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeGetProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
  if (gFake == FakeNext::LookupFails || std::string_view {name} != "xrEnumerateInstanceExtensionProperties") {
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  *function = reinterpret_cast<PFN_xrVoidFunction>(&FakeEnumerate);
  return XR_SUCCESS;
}

XrCompositionLayerQuad Quad(float z) {
  XrCompositionLayerQuad quad {XR_TYPE_COMPOSITION_LAYER_QUAD};
  quad.pose = {{0, 0, 0, 1}, {0, 0, z}};
  quad.size = {0.3f, 0.2f};
  return quad;
}

bool Near(float a, float b, float tolerance = 1e-4f) {
  return std::abs(a - b) <= tolerance;
}

}// namespace

int main() {
  Scenario("Layer room never underflows (XR-9)", [] {
    Check(LayerRoom(16, 1) == 15, "16 max, 1 game layer: 15");
    Check(LayerRoom(16, 16) == 0, "the game at the limit: 0");
    Check(LayerRoom(16, 20) == 0, "the game over the limit: 0, not 4 billion");
    Check(LayerRoom(0, 0) == 0, "a zero maximum: 0");
  });

  Scenario("Composition: game layers first, panels back to front, hands last", [] {
    FrameComposer composer {16, 8};
    XrCompositionLayerProjection game {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* gameLayers[] {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&game)};
    auto nearQuad = Quad(-0.4f);
    auto farQuad = Quad(-0.9f);
    auto midQuad = Quad(-0.6f);
    XrCompositionLayerProjection hands {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    ComposedPanel panels[] {{&nearQuad, 0.4f}, {&farQuad, 0.9f}, {&midQuad, 0.6f}};
    const auto result
      = composer.Compose(gameLayers, panels, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hands), 16);
    Check(result.mLayers.size() == 5, "1 game + 3 panels + hands");
    Check(result.mLayers[0] == gameLayers[0], "the game's layer is first, unchanged");
    Check(
      result.mLayers[1] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&farQuad)
        && result.mLayers[2] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&midQuad)
        && result.mLayers[3] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&nearQuad),
      "panels back to front (far, mid, near)");
    Check(result.mLayers[4] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hands), "hands last");
  });

  Scenario("No room: the farthest panels are dropped; hands keep their slot", [] {
    FrameComposer composer {16, 8};
    std::vector<XrCompositionLayerProjection> game(14, {XR_TYPE_COMPOSITION_LAYER_PROJECTION});
    std::vector<const XrCompositionLayerBaseHeader*> gameLayers;
    for (const auto& layer: game) {
      gameLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
    }
    auto nearQuad = Quad(-0.4f);
    auto farQuad = Quad(-0.9f);
    XrCompositionLayerProjection hands {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    ComposedPanel panels[] {{&nearQuad, 0.4f}, {&farQuad, 0.9f}};
    const auto result
      = composer.Compose(gameLayers, panels, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hands), 16);
    Check(result.mLayers.size() == 16, "exactly at the runtime's limit");
    Check(result.mPanelsDropped == 1 && result.mPanelsComposed == 1, "one panel dropped");
    Check(result.mLayers[14] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&nearQuad), "the nearer one kept");
    Check(result.mHandLayerComposed, "the hand layer kept its slot");

    std::vector<XrCompositionLayerProjection> full(16, {XR_TYPE_COMPOSITION_LAYER_PROJECTION});
    std::vector<const XrCompositionLayerBaseHeader*> fullLayers;
    for (const auto& layer: full) {
      fullLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
    }
    const auto atLimit = composer.Compose(fullLayers, panels, nullptr, 16);
    Check(atLimit.mLayers.size() == 16 && atLimit.mPanelsComposed == 0, "the game alone at the limit: nothing added");
  });

  Scenario("A focused panel wins a distance tie", [] {
    FrameComposer composer {16, 8};
    XrCompositionLayerProjection game {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* gameLayers[] {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&game)};
    auto a = Quad(-0.5f);
    auto b = Quad(-0.5f);
    ComposedPanel panels[] {{&a, 0.5f, true}, {&b, 0.5f, false}};
    const auto result = composer.Compose(gameLayers, panels, nullptr, 16);
    Check(result.mLayers[2] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&a), "the focused one is drawn on top");
  });

  Scenario("Hand-tracking probe is three-state (XR-7)", [] {
    gFake = FakeNext::HasHands;
    Check(ProbeInstanceExtension(&FakeGetProcAddr, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == Support::Supported, "listed: supported");
    gFake = FakeNext::NoHands;
    Check(ProbeInstanceExtension(&FakeGetProcAddr, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == Support::Unsupported, "not listed: unsupported");
    gFake = FakeNext::LookupFails;
    Check(ProbeInstanceExtension(&FakeGetProcAddr, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == Support::Unknown, "lookup failed: unknown, not 'no'");
    gFake = FakeNext::EnumerateFails;
    Check(ProbeInstanceExtension(&FakeGetProcAddr, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == Support::Unknown, "enumeration failed: unknown");
    Check(ProbeInstanceExtension(nullptr, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == Support::Unknown, "no next layer: unknown");
  });

  Scenario("Extension plans: added once, never when unsupported", [] {
    const char* game[] {"XR_KHR_D3D11_enable"};
    auto plan = PlanExtensions(game, Support::Supported);
    Check(plan.mNames.size() == 2 && plan.mLayerAddsHandTracking && plan.Who() == "the layer", "supported: the layer adds it");
    plan = PlanExtensions(game, Support::Unknown);
    Check(plan.mLayerAddsHandTracking, "unknown: requested anyway");
    plan = PlanExtensions(game, Support::Unsupported);
    Check(plan.mNames.size() == 1 && !plan.mLayerAddsHandTracking, "unsupported: not added");
    const char* gameWithHands[] {"XR_KHR_D3D11_enable", XR_EXT_HAND_TRACKING_EXTENSION_NAME};
    plan = PlanExtensions(gameWithHands, Support::Supported);
    Check(plan.mNames.size() == 2 && plan.Who() == "the game or a layer above us", "already requested above us: never added twice");
    plan = WithoutLayerAdditions(game);
    Check(plan.mNames.size() == 1 && !plan.mLayerAddsHandTracking, "the retry uses only the game's list");
  });

  Scenario("Layer settings: on/off, per-game list, test panel (VR-8)", [] {
    auto config = ParseLayerConfig(R"({"layer": {"disabledGames": ["Sim.EXE", 42, ""], "testPanel": true}})");
    Check(config.mEnabled && config.mTestPanel, "enabled, test panel on");
    Check(config.mDisabledGames.size() == 1, "bad entries skipped");
    Check(config.IsDisabledFor(L"sim.exe") && !config.IsDisabledFor(L"Elite.exe"), "matched by name, any case");
    config = ParseLayerConfig(R"({"layer": {"enabled": false}})");
    Check(config.IsDisabledFor(L"anything.exe"), "globally off");
    config = ParseLayerConfig("{garbage");
    Check(config.mEnabled && !config.mTestPanel, "unreadable settings: defaults");

    config = {};
    std::map<std::string, std::string> environment {
      {std::string {Product::LayerDisableEnvVar}, "1"}, {std::string {Product::TestPanelEnvVar}, "1"}};
    ApplyEnvironment(config, [&](std::string_view name) -> std::optional<std::string> {
      const auto it = environment.find(std::string {name});
      return it == environment.end() ? std::nullopt : std::optional {it->second};
    });
    Check(config.mDisabledByEnvironment && config.IsDisabledFor(L"game.exe"), "the disable variable turns it off");
    Check(config.mTestPanel, "the test-panel variable turns the panel on");
  });

  Scenario("Test card: corners say which way up it is", [] {
    TestPatternOptions options {.mWidth = 256, .mHeight = 160, .mGridSpacing = 32, .mCornerSize = 24, .mOrder = PixelOrder::RGBA};
    const auto pixels = MakeTestPattern(options);
    const auto at = [&](uint32_t x, uint32_t y) {
      const auto p = pixels[y * options.mWidth + x];
      return ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);// RGBA bytes -> 0xRRGGBB
    };
    Check(pixels.size() == 256u * 160u, "one pixel per texel");
    Check(at(5, 5) == TestPatternColors::TopLeft, "top-left is red");
    Check(at(250, 5) == TestPatternColors::TopRight, "top-right is green");
    Check(at(5, 155) == TestPatternColors::BottomLeft, "bottom-left is blue");
    Check(at(250, 155) == TestPatternColors::BottomRight, "bottom-right is white");
    Check(at(128, 80) == TestPatternColors::Target, "the touch target is in the middle");
    Check((pixels[0] >> 24) == 0xFF, "opaque");
  });

  Scenario("OpenXR helpers: names, validation, poses (XR-10)", [] {
    Check(XrUtil::ResultName(XR_ERROR_EXTENSION_NOT_PRESENT) == "XR_ERROR_EXTENSION_NOT_PRESENT", "own result names");
    Check(XrUtil::ResultName(static_cast<XrResult>(-999999)) == "XR_UNKNOWN_FAILURE", "unknown codes don't crash");

    XrSpaceLocation location {XR_TYPE_SPACE_LOCATION};
    location.locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    location.pose = XrUtil::IdentityPose;
    Check(XrUtil::IsUsable(location), "a valid pose is usable");
    location.pose.position.y = std::numeric_limits<float>::quiet_NaN();
    Check(!XrUtil::IsUsable(location), "a NaN is dropped");
    location.pose.position.y = std::numeric_limits<float>::infinity();
    Check(!XrUtil::IsUsable(location), "an infinity is dropped");
    location.pose = XrUtil::IdentityPose;
    location.locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    Check(!XrUtil::IsUsable(location), "no valid position: not usable");

    // Head 1.2 m up, turned 90 degrees left, looking down 50 degrees
    const auto yaw = XrUtil::FromYaw(std::numbers::pi_v<float> / 2);
    const XrQuaternionf pitch {std::sin(-0.87f / 2), 0, 0, std::cos(-0.87f / 2)};
    const XrPosef head {XrUtil::Multiply(yaw, pitch), {0, 1.2f, 0}};
    const auto placed = XrUtil::PlaceInFront(head, 0.5f, 0.2f);
    Check(
      Near(placed.position.x, -0.5f) && Near(placed.position.y, 1.0f) && Near(placed.position.z, 0),
      std::format("placed 0.5 m ahead, level, 0.2 m down ({:.2f}, {:.2f}, {:.2f})", placed.position.x, placed.position.y, placed.position.z));
    const auto facing = XrUtil::Rotate(placed.orientation, {0, 0, 1});
    Check(Near(facing.x, 1) && Near(facing.y, 0), "the panel faces back toward the head, and stays level");

    // ADR-037: after a recenter, panels are placed from the head, level
    const auto level = XrUtil::LevelPose(head);
    const auto up = XrUtil::Rotate(level.orientation, {0, 1, 0});
    const auto ahead = XrUtil::Rotate(level.orientation, {0, 0, -1});
    Check(Near(level.position.y, 1.2f) && Near(up.y, 1) && Near(ahead.x, -1) && Near(ahead.y, 0),
      "the level pose: at the head, upright, facing where the head faces (its yaw only)");

    const XrPosef panel {{0, 0, 0, 1}, {0, 1, -1}};
    Check(Near(XrUtil::DistanceToRect(panel, {0.4f, 0.2f}, {0, 1, 0}), 1), "straight in front: the plane distance");
    Check(Near(XrUtil::DistanceToRect(panel, {0.4f, 0.2f}, {0.5f, 1, -1}), 0.3f), "beside it: to the nearest edge");

    const XrPosef base {XrUtil::FromYaw(std::numbers::pi_v<float>), {1, 0, 0}};
    const auto composed = XrUtil::Compose(base, {{0, 0, 0, 1}, {0, 0, -1}});
    Check(Near(composed.position.x, 1) && Near(composed.position.z, 1), "composing poses rotates then translates");
  });

  Scenario("Gaze focus: the head's forward ray against a panel, with a tolerance (doc 05)", [] {
    const XrPosef head {{0, 0, 0, 1}, {0, 0, 0}};// looking down -Z
    const XrPosef ahead {{0, 0, 0, 1}, {0, 0, -1}};
    const XrExtent2Df size {0.4f, 0.3f};
    const auto hit = XrUtil::GazeDistance(head, ahead, size, 0);
    Check(hit && std::abs(*hit - 1.0f) < 1e-4f, "straight ahead, 1 m: hit at 1 m");
    const XrPosef aside {{0, 0, 0, 1}, {0.25f, 0, -1}};// its edge 5 cm to the right of the ray
    Check(!XrUtil::GazeDistance(head, aside, size, 0), "5 cm off its edge: a miss");
    Check(XrUtil::GazeDistance(head, aside, size, 3.0f * 3.14159265f / 180.0f).has_value(), "with 3 degrees (5.2 cm at 1 m): a hit");
    const XrPosef behind {{0, 0, 0, 1}, {0, 0, 1}};
    Check(!XrUtil::GazeDistance(head, behind, size, 0.1f), "behind the head: never");
    const XrPosef turned {XrUtil::FromYaw(1.5707963f), {0, 0, -1}};// edge-on
    Check(!XrUtil::GazeDistance(head, turned, size, 0), "edge-on: a miss");
  });

  Scenario("Where hands fall in an eye's image: drawn and cleared only there (XR-13)", [] {
    const XrPosef eye {{0, 0, 0, 1}, {0, 0, 0}};// looking down -Z
    const XrFovf fov {-0.785398f, 0.785398f, 0.785398f, -0.785398f};// 90 degrees each way: tangents -1 to 1
    const auto in = [&](std::initializer_list<Sphere> spheres, const XrPosef& from = XrPosef {{0, 0, 0, 1}, {0, 0, 0}}) {
      return SpheresInView(from, fov, 1000, 1000, std::span<const Sphere> {spheres.begin(), spheres.size()}, 0);
    };
    const auto ahead = in({{{0, 0, -1}, 0.01f}});
    Check(!ahead.IsEmpty() && ahead.mLeft >= 490 && ahead.mRight <= 510 && ahead.mTop >= 490 && ahead.mBottom <= 510,
      std::format("a 1 cm sphere 1 m ahead: about 10 px wide in the middle ({}, {}, {}, {})", ahead.mLeft, ahead.mTop,
        ahead.mRight, ahead.mBottom));
    const auto right = in({{{0.5f, 0.25f, -1}, 0.01f}});
    Check(!right.IsEmpty() && right.mLeft > 740 && right.mRight < 760 && right.mTop > 365 && right.mBottom < 385,
      "right and up: right of the middle, above it (y grows downward)");
    Check(in({{{0, 0, 1}, 0.05f}}).IsEmpty(), "behind the eye: nothing");
    Check(in({{{3, 0, -1}, 0.05f}}).IsEmpty(), "far outside the field of view: nothing");
    const auto edge = in({{{1.0f, 0, -1}, 0.05f}});
    Check(!edge.IsEmpty() && edge.mRight == 1000 && edge.mLeft > 900, "across the right edge: up to the edge");
    const auto whole = in({{{0, 0, -0.02f}, 0.05f}});
    Check(whole == PixelRect {0, 0, 1000, 1000}, "reaching the eye's plane: the whole view");
    const auto two = in({{{-0.2f, 0, -1}, 0.01f}, {{0.2f, 0, -1}, 0.01f}});
    Check(two.mLeft < 410 && two.mRight > 590, "two spheres: one rectangle around both");
    // An eye turned 90 degrees to the left sees what's to the left
    const XrPosef turned {XrUtil::FromYaw(1.5707963f), {0, 0, 0}};
    const auto left = in({{{-1, 0, 0}, 0.01f}}, turned);
    Check(!left.IsEmpty() && left.mLeft >= 490 && left.mRight <= 510, "a turned eye: the sphere it faces is in the middle");
    const auto margin = SpheresInView(eye, fov, 1000, 1000, std::array<Sphere, 1> {{{{0, 0, -1}, 0.01f}}}, 4);
    Check(margin.mLeft == ahead.mLeft - 4 && margin.mBottom == ahead.mBottom + 4, "the margin grows it on every side");
    const Sphere bad {{std::numeric_limits<float>::quiet_NaN(), 0, -1}, 0.01f};
    Check(SpheresInView(eye, fov, 1000, 1000, std::array<Sphere, 1> {bad}, 0).IsEmpty(), "a NaN joint adds nothing");

    Check(Union(PixelRect {}, PixelRect {1, 2, 3, 4}) == PixelRect {1, 2, 3, 4}, "an empty rectangle adds nothing to a union");
    Check(Union(PixelRect {0, 0, 10, 10}, PixelRect {5, 5, 20, 20}) == PixelRect {0, 0, 20, 20}, "a union holds both");
    Check(Pad(PixelRect {0, 5, 10, 10}, 2, 11, 100) == PixelRect {0, 3, 11, 12}, "padding stays within the image");
  });

  Scenario("Known layers are told apart by name (CMP-1, CMP-2)", [] {
    using namespace OpenDesktopVR::KnownLayers;
    Check(Classify("XR_APILAYER_ULTRALEAP_hand_tracking") == Ultraleap, "Ultraleap");
    Check(Classify("C:\\Program Files\\Ultraleap\\OpenXR\\UltraleapHandTracking.json") == Ultraleap, "Ultraleap by its path");
    Check(Classify("XR_APILAYER_NOVENDOR_XRNeckSafer") == PoseChanging, "XRNeckSafer: pose-changing");
    Check(Classify("XR_APILAYER_NOVENDOR_motion_compensation") == PoseChanging, "motion compensation: pose-changing");
    Check(Classify("XR_APILAYER_NOVENDOR_OBSMirror") == MirrorCapture, "a mirror layer");
    Check(Classify("XR_APILAYER_NOVENDOR_toolkit") == None, "anything else: no advice");
    Check(Classify(std::string {Product::LayerName}) == None, "ours is none of them");
  });

  Scenario("The chain below ours, from the loader's entries (M6c)", [] {
    using namespace OpenDesktopVR::KnownLayers;
    const auto entry = [](const char* name) {
      XrApiLayerNextInfo info {XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
      strcpy_s(info.layerName, name);
      return info;
    };
    auto ours = entry(std::string {Product::LayerName}.c_str());
    auto neck = entry("XR_APILAYER_NOVENDOR_XRNeckSafer");
    auto leap = entry("XR_APILAYER_ULTRALEAP_hand_tracking");
    ours.next = &neck;
    neck.next = &leap;
    const auto summary = Layer::SummarizeChain(&ours, Product::LayerName);
    Check(summary.mBelow.size() == 2 && summary.mBelow[0] == "XR_APILAYER_NOVENDOR_XRNeckSafer", "ours is skipped; nearest first");
    Check(summary.mKnownBelow == (PoseChanging | Ultraleap), "both kinds are below");
    Check(summary.mNearestRuntime == Ultraleap, "Ultraleap is last, nearest the runtime");
    Check(summary.Describe() == "XR_APILAYER_NOVENDOR_XRNeckSafer, XR_APILAYER_ULTRALEAP_hand_tracking", "described in order");

    ours.next = nullptr;
    Check(Layer::SummarizeChain(&ours, Product::LayerName).Describe() == "none", "nothing below: none");
    Check(Layer::SummarizeChain(&neck, Product::LayerName).mBelow.size() == 2, "a chain starting below ours is read whole");
    Check(Layer::SummarizeChain(nullptr, Product::LayerName).mBelow.empty(), "no chain: nothing");

    // A malformed, circular chain is read at most 64 entries
    leap.next = &neck;
    ours.next = &neck;
    Check(Layer::SummarizeChain(&ours, Product::LayerName).mBelow.size() == 64, "a circular chain stops at 64 entries");

    const char* extensions[] {"XR_KHR_D3D11_enable", XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
    Check(Layer::EnablesExtension(extensions, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME), "the depth extension is found");
    Check(!Layer::EnablesExtension({extensions, 1}, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME), "and not when absent");
  });

  return Finish();
}
