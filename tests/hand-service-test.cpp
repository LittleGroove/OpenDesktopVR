// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The host's hand thread (M4a) with a fake tracking channel and synthetic
// hands: samples in order, a tap, two-finger scrolling (natural and
// reversed), tracking gaps during a drag (XR-19: 300 ms lets go once and
// doesn't click again on return; 100 ms doesn't end the drag), both with no
// samples and with one hand untracked while samples keep coming, hands turned
// off mid-touch, the hand state the layer reads, the settings format.
// M5b: typing on the keyboard panel (one press per poke, hover never types,
// never a pointer touch, both hands drawn), a hand at the keyboard doesn't
// point at the panel behind it, and the press-count baseline starts afresh
// whenever the keyboard shows again (VK-4, VK-6, K2).

#include "check.hpp"
#include "synthetic-hands.hpp"

#include <OpenDesktopVR/HandConfig.hpp>
#include <OpenDesktopVR/HandService.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Host;
using namespace OpenDesktopVR::SyntheticHands;
using namespace std::chrono_literals;
using Testing::Check;
using Testing::Scenario;

namespace {

constexpr uint64_t PanelID = 7;
constexpr float PanelZ = -0.5f;

// The ring, in memory: the last 8 samples readable, like the shared one
struct FakeChannel final : TrackingChannel {
  HANDLE mEvent {CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  mutable std::mutex mMutex;
  std::vector<SharedAbi::TrackingSample> mSamples;
  std::vector<SharedAbi::HandState> mStates;

  ~FakeChannel() override {
    CloseHandle(mEvent);
  }
  void* Event() const noexcept override {
    return mEvent;
  }
  uint64_t Written() const noexcept override {
    std::scoped_lock lock(mMutex);
    return mSamples.size();
  }
  bool Read(uint64_t index, SharedAbi::TrackingSample& out) const noexcept override {
    std::scoped_lock lock(mMutex);
    if (index == 0 || index > mSamples.size() || mSamples.size() - index >= SharedAbi::TrackingSlots) {
      return false;
    }
    out = mSamples[index - 1];
    return true;
  }
  void WriteHandState(const SharedAbi::HandState& state) noexcept override {
    std::scoped_lock lock(mMutex);
    mStates.push_back(state);
  }
  void Push(SharedAbi::TrackingSample sample) {
    {
      std::scoped_lock lock(mMutex);
      sample.mIndex = mSamples.size() + 1;
      mSamples.push_back(sample);
    }
    SetEvent(mEvent);
  }
  SharedAbi::HandState LastState() const {
    std::scoped_lock lock(mMutex);
    return mStates.empty() ? SharedAbi::HandState {} : mStates.back();
  }
  bool AnyState(const std::function<bool(const SharedAbi::HandState&)>& test) const {
    std::scoped_lock lock(mMutex);
    return std::any_of(mStates.begin(), mStates.end(), test);
  }
};

struct RecordingSink final : PointerSink {
  std::mutex mMutex;
  std::vector<HandInteraction::CursorUpdate> mUpdates;
  void Pointer(const TouchPanel*, const HandInteraction::CursorUpdate& update) override {
    std::scoped_lock lock(mMutex);
    mUpdates.push_back(update);
  }
  // How many times the touch started and ended
  std::pair<int, int> Presses() {
    std::scoped_lock lock(mMutex);
    int downs = 0;
    int ups = 0;
    bool down = false;
    for (const auto& update: mUpdates) {
      const bool touching = update.mState == HandInteraction::TouchState::Touching;
      downs += (touching && !down) ? 1 : 0;
      ups += (!touching && down) ? 1 : 0;
      down = touching;
    }
    return {downs, ups};
  }
  int32_t Wheel() {
    std::scoped_lock lock(mMutex);
    int32_t total = 0;
    for (const auto& update: mUpdates) {
      total += update.mWheel;
    }
    return total;
  }
};

// What the keyboard service does with typing results, counted
struct RecordingTyping final : TypingSink {
  std::mutex mMutex;
  int mResults {0};
  int mAppeared {0};
  int mStopped {0};
  int mPresses {0};
  std::array<uint8_t, 2> mBaseline {};
  void Typing(const TouchPanel&, const HandInteraction::TypingResult& result, bool appeared) override {
    std::scoped_lock lock(mMutex);
    ++mResults;
    if (appeared) {
      ++mAppeared;
      for (std::size_t h = 0; h < 2; ++h) {
        mBaseline[h] = result.mFingers[h].mPressCount;
      }
    }
    for (std::size_t h = 0; h < 2; ++h) {
      const auto& finger = result.mFingers[h];
      if (finger.mState == HandInteraction::TypingFingerState::Pressed && finger.mPressCount != mBaseline[h]) {
        ++mPresses;
      }
      mBaseline[h] = finger.mPressCount;
    }
  }
  void TypingStopped() override {
    std::scoped_lock lock(mMutex);
    ++mStopped;
  }
  void Tick() override {
  }
  int Presses() {
    std::scoped_lock lock(mMutex);
    return mPresses;
  }
};

constexpr uint64_t KeyboardID = 99;

TouchPanel KeyboardPanelAt(float x, float z) {
  TouchPanel keyboard {.mID = KeyboardID, .mName = L"Keyboard", .mTouch = false, .mScroll = false, .mKeyboard = true};
  keyboard.mGeometry
    = {.mID = KeyboardID, .mPosition = {x, 0, z}, .mOrientation = Quaternion::Identity, .mSize = {0.3f, 0.1f}};
  return keyboard;
}

std::shared_ptr<const TouchPanels> OnePanel() {
  TouchPanel panel {.mID = PanelID, .mName = L"Test"};
  panel.mGeometry = {.mID = PanelID, .mPosition = {0, 0, PanelZ}, .mOrientation = Quaternion::Identity, .mSize = {0.4f, 0.3f}};
  return std::make_shared<const TouchPanels>(TouchPanels {panel});
}

// A right hand whose contact point (the index tip, or between two fingers)
// is `depth` in front of the panel at (x, y) on it
HandJoints RightHand(Pose pose, float x, float y, float depth) {
  const Vector3 contact {x, y, PanelZ + depth};
  auto hand = MakeHand(pose, PokeBasis, contact);
  if (pose == Pose::TwoFingers) {
    const auto middle = (hand[Joint::IndexTip].mPosition + hand[Joint::MiddleTip].mPosition) * 0.5f;
    for (auto& joint: hand.mJoints) {
      joint.mPosition += contact - middle;
    }
  }
  hand.mIsTracked = true;
  return hand;
}

// Feeds samples at 90 a second (display time), waiting for each to be read
struct Feeder {
  FakeChannel& mChannel;
  HandService& mService;
  int64_t mTime {1'000'000'000};

  void Send(const HandJoints& right) {
    SharedAbi::TrackingSample sample {};
    sample.mDisplayTime = mTime;
    sample.mFlags = SharedAbi::SampleFlags::HeadValid;
    sample.mHead[6] = 1;// at the origin, looking down -Z
    if (right.mIsTracked) {
      auto& out = sample.mHands[1];
      out.mTracked = 1;
      for (std::size_t j = 0; j < SharedAbi::HandJointCount; ++j) {
        const auto& joint = right.mJoints[j];
        const float pose[7] {joint.mPosition.x, joint.mPosition.y, joint.mPosition.z, joint.mOrientation.x,
          joint.mOrientation.y, joint.mOrientation.z, joint.mOrientation.w};
        std::copy(std::begin(pose), std::end(pose), out.mJoints[j].mPose);
        out.mJoints[j].mRadius = joint.mRadius;
      }
    }
    const auto before = mService.Stats().mSamples;
    mChannel.Push(sample);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (mService.Stats().mSamples == before && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    mTime += 11'111'111;
  }
  // `seconds` of samples, moving from `from(t)` with t from 0 to 1
  void Path(double seconds, const std::function<HandJoints(float)>& at) {
    const int steps = std::max(1, static_cast<int>(seconds * 90));
    for (int i = 0; i <= steps; ++i) {
      Send(at(static_cast<float>(i) / static_cast<float>(steps)));
    }
  }
  void Skip(std::chrono::milliseconds gap) {
    mTime += std::chrono::duration_cast<std::chrono::nanoseconds>(gap).count();
  }
};

float Lerp(float a, float b, float t) {
  return a + ((b - a) * t);
}

// A right pointing hand with its index tip at (x, y, z)
HandJoints RightHandAt(float x, float y, float z) {
  auto hand = MakeHand(Pose::Point, PokeBasis, {x, y, z});
  hand.mIsTracked = true;
  return hand;
}

// Hover 3 cm in front of a plane at `z`, poke 8 mm through it, pull back
void Poke(Feeder& feed, float x, float z) {
  feed.Path(0.2, [&](float) { return RightHandAt(x, 0, z + 0.03f); });
  feed.Path(0.12, [&](float t) { return RightHandAt(x, 0, z + Lerp(0.03f, -0.008f, t)); });
  feed.Path(0.08, [&](float) { return RightHandAt(x, 0, z - 0.008f); });
  feed.Path(0.15, [&](float t) { return RightHandAt(x, 0, z + Lerp(-0.008f, 0.03f, t)); });
}

}// namespace

int main() {
  Scenario("A tap: one press and one release on the panel; the hand shows while pointing (doc 05)", [] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    feed.Path(0.3, [](float) { return RightHand(Pose::Point, 0, 0, 0.08f); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::Point, 0, 0, Lerp(0.08f, -0.012f, t)); });
    feed.Path(0.05, [](float) { return RightHand(Pose::Point, 0, 0, -0.012f); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::Point, 0, 0, Lerp(-0.012f, 0.08f, t)); });
    service.Stop();
    const auto [downs, ups] = sink.Presses();
    Check(downs == 1 && ups == 1, std::format("one press, one release ({} and {})", downs, ups));
    const auto stats = service.Stats();
    Check(stats.mTouches == 1 && stats.mTaps == 1, std::format("counted as one tap ({} touches, {} taps)", stats.mTouches, stats.mTaps));
    Check(channel.AnyState([](const auto& s) { return s.mVisibility[1] > 0.9f && (s.mFlags & SharedAbi::HandFlags::WantsSamples); }),
      "the layer was told to show the right hand, and to send samples");
    Check(channel.AnyState([](const auto& s) { return s.mVisibility[0] == 0; }), "the untracked left hand stays hidden");
    Check(channel.LastState().mFlags == 0, "stopping tells the layer to stop sending samples");
  });

  Scenario("Show my hand while pointing off: hidden, but taps still work", [] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({.mShowWhilePointing = false});
    service.Start();
    Feeder feed {channel, service};
    feed.Path(0.3, [](float) { return RightHand(Pose::Point, 0, 0, 0.08f); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::Point, 0, 0, Lerp(0.08f, -0.012f, t)); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::Point, 0, 0, Lerp(-0.012f, 0.08f, t)); });
    service.Stop();
    Check(!channel.AnyState([](const auto& s) { return s.mVisibility[1] > 0; }), "never shown");
    Check(sink.Presses() == std::pair {1, 1}, "still one tap");
  });

  // Touch, hold, drag sideways 3 cm: dragging
  const auto startDrag = [](Feeder& feed) {
    feed.Path(0.3, [](float) { return RightHand(Pose::Point, 0, 0, 0.08f); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::Point, 0, 0, Lerp(0.08f, -0.012f, t)); });
    feed.Path(0.4, [](float) { return RightHand(Pose::Point, 0, 0, -0.012f); });
    feed.Path(0.3, [](float t) { return RightHand(Pose::Point, Lerp(0, 0.03f, t), 0, -0.012f); });
  };

  Scenario("A 300 ms tracking gap during a drag lets go once; no click when tracking returns (XR-19)", [&] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    startDrag(feed);
    Check(sink.Presses() == std::pair {1, 0}, "dragging: pressed, not released");
    // No samples (the layer had no panels, or tracking was lost)
    std::this_thread::sleep_for(400ms);
    feed.Skip(300ms);
    Check(sink.Presses() == std::pair {1, 1}, "released during the gap, without a new sample");
    Check(service.Stats().mResets == 1, "counted as a let-go after tracking stopped");
    // Back, still pushed into the panel: not a new press
    feed.Path(0.3, [](float t) { return RightHand(Pose::Point, Lerp(0.03f, 0.05f, t), 0, -0.012f); });
    feed.Path(0.2, [](float t) { return RightHand(Pose::Point, 0.05f, 0, Lerp(-0.012f, 0.08f, t)); });
    service.Stop();
    const auto [downs, ups] = sink.Presses();
    Check(downs == 1 && ups == 1, std::format("no click on return ({} presses, {} releases in all)", downs, ups));
  });

  Scenario("A 100 ms tracking gap during a drag doesn't end it (XR-19)", [&] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    startDrag(feed);
    std::this_thread::sleep_for(100ms);
    feed.Skip(100ms);
    feed.Path(0.2, [](float t) { return RightHand(Pose::Point, Lerp(0.03f, 0.05f, t), 0, -0.012f); });
    Check(sink.Presses() == std::pair {1, 0}, "still pressed after the gap");
    feed.Path(0.2, [](float t) { return RightHand(Pose::Point, 0.05f, 0, Lerp(-0.012f, 0.08f, t)); });
    service.Stop();
    Check(sink.Presses() == std::pair {1, 1}, "released only when pulled back");
    const auto stats = service.Stats();
    Check(stats.mResets == 0 && stats.mDrags == 1, std::format("one drag, no let-go ({} drags, {} resets)", stats.mDrags, stats.mResets));
  });

  // The same rule for one hand whose tracking is lost while samples keep
  // coming (fingers hidden from the headset's cameras): doc 03 lists "hand
  // tracking off or lost" with the other 250 ms causes. Found in M4c: the
  // imported tracker lets go at once, so the host bridges short losses.
  const HandJoints lost {};
  Scenario("One hand untracked for 100 ms during a drag, samples still coming: the drag continues (XR-19)", [&] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    startDrag(feed);
    feed.Path(0.1, [&](float) { return lost; });
    Check(sink.Presses() == std::pair {1, 0}, "still pressed while the hand is lost");
    feed.Path(0.2, [](float t) { return RightHand(Pose::Point, Lerp(0.03f, 0.05f, t), 0, -0.012f); });
    Check(sink.Presses() == std::pair {1, 0}, "still pressed after it's back, and dragging on");
    feed.Path(0.2, [](float t) { return RightHand(Pose::Point, 0.05f, 0, Lerp(-0.012f, 0.08f, t)); });
    service.Stop();
    Check(sink.Presses() == std::pair {1, 1}, "released only when pulled back");
    const auto stats = service.Stats();
    Check(stats.mResets == 0 && stats.mDrags == 1, std::format("one drag, no let-go ({} drags, {} resets)", stats.mDrags, stats.mResets));
  });

  Scenario("One hand untracked for 300 ms during a drag: let go once, after 250 ms; no click on return (XR-19)", [&] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    startDrag(feed);
    feed.Path(0.2, [&](float) { return lost; });
    Check(sink.Presses() == std::pair {1, 0}, "still pressed 200 ms in");
    feed.Path(0.1, [&](float) { return lost; });
    Check(sink.Presses() == std::pair {1, 1}, "let go by 300 ms");
    Check(service.Stats().mResets == 1, "counted as a let-go after tracking stopped");
    // Back, still pushed into the panel: not a new press
    feed.Path(0.3, [](float t) { return RightHand(Pose::Point, Lerp(0.03f, 0.05f, t), 0, -0.012f); });
    feed.Path(0.2, [](float t) { return RightHand(Pose::Point, 0.05f, 0, Lerp(-0.012f, 0.08f, t)); });
    service.Stop();
    const auto [downs, ups] = sink.Presses();
    Check(downs == 1 && ups == 1, std::format("no click on return ({} presses, {} releases in all)", downs, ups));
  });

  Scenario("A hovering hand that loses tracking is let go at once (only touches are held)", [&] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    feed.Path(0.3, [](float) { return RightHand(Pose::Point, 0, 0, 0.05f); });
    feed.Send(lost);
    service.Stop();
    std::scoped_lock lock(sink.mMutex);
    Check(!sink.mUpdates.empty() && sink.mUpdates.back().mState == HandInteraction::TouchState::NotNear,
      "the pointer left the panel on the first untracked sample");
  });

  Scenario("Hands turned off mid-touch: let go at once, and the layer stops sending samples", [&] {
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    startDrag(feed);
    service.SetSettings({.mEnabled = false});
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (sink.Presses().second == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    Check(sink.Presses() == std::pair {1, 1}, "released");
    Check(channel.LastState().mFlags == 0 && channel.LastState().mVisibility[1] == 0, "hand hidden, no samples wanted");
    service.Stop();
  });

  const auto scroll = [](Feeder& feed) {
    feed.Path(0.3, [](float) { return RightHand(Pose::TwoFingers, 0, 0.05f, 0.08f); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::TwoFingers, 0, 0.05f, Lerp(0.08f, -0.005f, t)); });
    feed.Path(0.5, [](float t) { return RightHand(Pose::TwoFingers, 0, Lerp(0.05f, 0.05f - 0.036f, t), -0.005f); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::TwoFingers, 0, 0.05f - 0.036f, Lerp(-0.005f, 0.08f, t)); });
  };

  Scenario("Two fingers dragged down 36 mm: about 3 notches, natural; reversed when asked; no click (doc 05)", [&] {
    int32_t natural = 0;
    {
      FakeChannel channel;
      RecordingSink sink;
      HandService service {channel, [] { return OnePanel(); }, &sink};
      service.SetSettings({});
      service.Start();
      Feeder feed {channel, service};
      scroll(feed);
      service.Stop();
      natural = sink.Wheel();
      Check(natural >= 240 && natural <= 360, std::format("natural: content follows the fingers, positive ({})", natural));
      Check(service.Stats().mTaps == 0, "not a tap");
    }
    FakeChannel channel;
    RecordingSink sink;
    HandService service {channel, [] { return OnePanel(); }, &sink};
    service.SetSettings({.mReverseScroll = true});
    service.Start();
    Feeder feed {channel, service};
    scroll(feed);
    service.Stop();
    Check(sink.Wheel() == -natural, std::format("reversed: the same amount the other way ({})", sink.Wheel()));
  });

  Scenario("A panel with touch and scroll off can't be touched", [] {
    FakeChannel channel;
    RecordingSink sink;
    auto panels = std::make_shared<TouchPanels>(*OnePanel());
    (*panels)[0].mTouch = false;
    (*panels)[0].mScroll = false;
    std::shared_ptr<const TouchPanels> shared = panels;
    HandService service {channel, [shared] { return shared; }, &sink};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    feed.Path(0.3, [](float) { return RightHand(Pose::Point, 0, 0, 0.08f); });
    feed.Path(0.15, [](float t) { return RightHand(Pose::Point, 0, 0, Lerp(0.08f, -0.012f, t)); });
    service.Stop();
    Check(sink.Presses().first == 0, "no press");
  });

  Scenario("Samples the thread fell behind on are skipped, never misread", [] {
    FakeChannel channel;
    HandService service {channel, [] { return OnePanel(); }, nullptr};
    // Twenty samples before the thread starts: only the last 8 are readable
    for (int i = 0; i < 20; ++i) {
      SharedAbi::TrackingSample sample {};
      sample.mDisplayTime = 1'000'000'000 + (i * 11'111'111LL);
      channel.Push(sample);
    }
    service.SetSettings({});
    service.Start();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (service.Stats().mSamples < 8 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    service.Stop();
    const auto stats = service.Stats();
    Check(stats.mSamples == 8 && stats.mSkipped == 12, std::format("8 read, 12 skipped ({}, {})", stats.mSamples, stats.mSkipped));
  });

  Scenario("Typing on the keyboard: a press per poke, hover never types, no pointer touch, both hands drawn (VK-6, K2)", [] {
    FakeChannel channel;
    RecordingSink sink;
    RecordingTyping typing;
    // The keyboard beside the panel, in the same plane
    const auto panels = [] {
      auto list = *OnePanel();
      list.push_back(KeyboardPanelAt(0.5f, PanelZ));
      return std::make_shared<const TouchPanels>(std::move(list));
    }();
    HandService service {channel, [&] { return panels; }, &sink, {}, &typing};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    // Hovering over a key, 2 cm out, drifting: never a press
    feed.Path(0.6, [](float t) { return RightHandAt(0.5f + Lerp(0, 0.02f, t), 0, PanelZ + 0.02f); });
    Check(typing.Presses() == 0, std::format("hovering over the keys typed nothing ({} presses)", typing.Presses()));
    Poke(feed, 0.48f, PanelZ);
    Poke(feed, 0.52f, PanelZ);
    feed.Path(0.3, [](float) { return RightHandAt(0.5f, 0, PanelZ + 0.05f); });
    service.Stop();
    Check(typing.Presses() == 2, std::format("two pokes, two presses ({})", typing.Presses()));
    Check(typing.mAppeared == 1, std::format("the keyboard appeared once ({})", typing.mAppeared));
    const auto [downs, ups] = sink.Presses();
    Check(downs == 0, std::format("the keyboard is never a pointer target ({} pointer presses)", downs));
    Check(channel.AnyState([](const auto& state) { return state.mVisibility[0] == 1.0f && state.mVisibility[1] == 1.0f; }),
      "both hands are drawn while the keyboard shows, pointing or not (K2)");
  });

  Scenario("A hand at the keyboard doesn't point at the panel behind it (doc 05)", [] {
    // The keyboard 3 cm in front of the panel; a poke 2.5 cm through the
    // keyboard ends 5 mm in front of the panel, inside its touch distance
    const auto poke = [](Feeder& feed) {
      feed.Path(0.3, [](float) { return RightHandAt(0, 0, PanelZ + 0.07f); });
      feed.Path(0.2, [](float t) { return RightHandAt(0, 0, PanelZ + Lerp(0.07f, 0.005f, t)); });
      feed.Path(0.1, [](float) { return RightHandAt(0, 0, PanelZ + 0.005f); });
      feed.Path(0.2, [](float t) { return RightHandAt(0, 0, PanelZ + Lerp(0.005f, 0.07f, t)); });
    };
    {
      // Without the keyboard, that poke touches the panel
      FakeChannel channel;
      RecordingSink sink;
      HandService service {channel, [] { return OnePanel(); }, &sink};
      service.SetSettings({});
      service.Start();
      Feeder feed {channel, service};
      poke(feed);
      service.Stop();
      Check(sink.Presses().first == 1, std::format("without the keyboard, the poke touches the panel ({})", sink.Presses().first));
    }
    FakeChannel channel;
    RecordingSink sink;
    RecordingTyping typing;
    const auto panels = [] {
      auto list = *OnePanel();
      list.push_back(KeyboardPanelAt(0, PanelZ + 0.03f));
      return std::make_shared<const TouchPanels>(std::move(list));
    }();
    HandService service {channel, [&] { return panels; }, &sink, {}, &typing};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    poke(feed);
    service.Stop();
    Check(typing.Presses() == 1, std::format("with it, the poke types one key ({})", typing.Presses()));
    Check(sink.Presses().first == 0, std::format("and never touches the panel behind ({})", sink.Presses().first));
  });

  Scenario("The keyboard hidden and shown again: let go, and the press counts start afresh (VK-4)", [] {
    FakeChannel channel;
    RecordingSink sink;
    RecordingTyping typing;
    std::atomic<bool> showing {true};
    const auto with = [] {
      auto list = *OnePanel();
      list.push_back(KeyboardPanelAt(0.5f, PanelZ));
      return std::make_shared<const TouchPanels>(std::move(list));
    }();
    const auto without = OnePanel();
    HandService service {channel, [&] { return showing ? with : without; }, &sink, {}, &typing};
    service.SetSettings({});
    service.Start();
    Feeder feed {channel, service};
    Poke(feed, 0.5f, PanelZ);
    showing = false;
    feed.Path(0.3, [](float) { return RightHandAt(0.5f, 0, PanelZ + 0.03f); });
    showing = true;
    // Back, with the fingertip already resting in the touch zone: no press
    feed.Path(0.4, [](float) { return RightHandAt(0.5f, 0, PanelZ - 0.004f); });
    feed.Path(0.3, [](float t) { return RightHandAt(0.5f, 0, PanelZ + Lerp(-0.004f, 0.05f, t)); });
    Poke(feed, 0.5f, PanelZ);
    service.Stop();
    Check(typing.mStopped >= 1, std::format("hiding it let go of the keys ({})", typing.mStopped));
    Check(typing.mAppeared == 2, std::format("shown again, it started afresh ({} times)", typing.mAppeared));
    Check(typing.Presses() == 2, std::format("one press before and one after; nothing from coming back ({})", typing.Presses()));
  });

  Scenario("Hand settings in settings.json: defaults, round trip, odd values (ST-4, ST-5)", [] {
    using JsonUtil::Json;
    const auto defaults = HandConfig::Read(Json::object());
    Check(defaults == HandSettings {}, "missing: the defaults (on, shown while pointing, no offset, natural scroll)");
    Json settings = Json::object();
    settings["hands"] = Json::object();
    settings["hands"]["future"] = 3;
    const HandSettings changed {.mEnabled = false, .mShowWhilePointing = false, .mTapOffsetUpMillimeters = 4.5f, .mReverseScroll = true};
    HandConfig::Write(settings, changed);
    Check(HandConfig::Read(settings) == changed, "written, then read back the same");
    Check(settings["hands"]["future"] == 3, "a key this version doesn't know survives");
    settings["hands"]["tapOffsetUpMillimeters"] = 500;
    settings["hands"]["enabled"] = "yes";
    const auto odd = HandConfig::Read(settings);
    Check(odd.mTapOffsetUpMillimeters == HandSettings::MaxTapOffsetMillimeters, "an offset too large is brought into range");
    Check(odd.mEnabled, "a malformed switch reads as its default");
  });

  return Testing::Finish();
}
