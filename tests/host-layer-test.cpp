// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// M2 end to end: the real host app (child processes) and the real layer DLL
// (in a fake game on a fake runtime), under a private instance name so a
// real host on the same PC is never touched.
//
//   host-layer-test <layer DLL> <host exe>
//
// Covers the M2 exit criteria that don't need a headset:
// - panels appear; the animated one is copied at its cap, the static one
//   once ("unchanged panels are not copied");
// - the host killed mid-session: panels go, the game never stalls; the host
//   restarted: panels come back;
// - a version mismatch in shared memory: pass through, one log line (SY-7);
// - the host's fence 100 ms late: frames keep the previous image, forced
//   waits are a minority, and the game's CPU never waits (ADR-012);
// - the host's GPU work never completing: no GPU wait at all (ADR-024);
// - static panels: nothing copied while nothing changes (bench gate);
// - an exited host's block, held open by another viewer: never connected to
//   (test round 5);
// - panels resized every frame: no swapchain re-created (RN-7);
// - gaze focus: the head turned to a panel, the layer reports it and wakes
//   the host; turned away, focus clears (PN-4, doc 05);
// - a panel's look: tinted red at half brightness, its pixels in the game's
//   swapchain have no green or blue left (PN-5);
// - a keybind (F24, sent from outside) hides and shows all panels in the
//   game: Raw Input, the host's input and UI threads, the renderer, the
//   layer (PN-6, KY-1, KY-3).

#include "check.hpp"
#include "fake-openxr.hpp"

#include <OpenDesktopVR/Automation.hpp>
#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/Seqlock.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <set>
#include <thread>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::FakeOpenXR;
using namespace OpenDesktopVR::Testing;
using namespace std::chrono_literals;

namespace {

std::wstring gHostExe;

struct HostProcess {
  PROCESS_INFORMATION mInfo {};
  explicit HostProcess(const std::wstring& extraArgs) {
    auto command = std::format(L"\"{}\" --no-register --warp --exit-after 120 {}", gHostExe, extraArgs);
    STARTUPINFOW startup {sizeof(startup)};
    if (!CreateProcessW(gHostExe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &mInfo)) {
      mInfo = {};
    }
  }
  bool IsRunning() const {
    return mInfo.hProcess && WaitForSingleObject(mInfo.hProcess, 0) == WAIT_TIMEOUT;
  }
  void Kill() {
    if (mInfo.hProcess) {
      TerminateProcess(mInfo.hProcess, 1);
      WaitForSingleObject(mInfo.hProcess, 5000);
    }
  }
  ~HostProcess() {
    Kill();
    if (mInfo.hProcess) {
      CloseHandle(mInfo.hProcess);
      CloseHandle(mInfo.hThread);
    }
  }
};

struct FrameRun {
  int mFrames {0};
  double mMaxEndFrameMs {0};
};

// Ends frames at about 90 Hz until `done` says stop or `limit` passes
FrameRun RunFrames(Game& game, std::chrono::milliseconds limit, const std::function<bool()>& done = {}) {
  FrameRun run;
  const auto start = TimeUtil::Now();
  auto next = start;
  while (TimeUtil::Now() - start < limit) {
    const auto before = TimeUtil::Now();
    game.EndFrame();
    run.mMaxEndFrameMs = std::max(run.mMaxEndFrameMs, TimeUtil::ToMilliseconds(TimeUtil::Now() - before));
    ++run.mFrames;
    if (done && done()) {
      break;
    }
    next += 11ms;
    std::this_thread::sleep_until(next);
  }
  return run;
}

int Quads() {
  return CountType(XR_TYPE_COMPOSITION_LAYER_QUAD);
}

// The largest green or blue value and the largest red one, over the first
// image of a swapchain (a static panel is copied once, into image 0)
struct ChannelPeaks {
  int mRed {-1};
  int mGreenOrBlue {-1};
};
ChannelPeaks Peaks(XrSwapchain swapchain, uint32_t width, uint32_t height) {
  const auto images = gRT.mSwapchains.find(reinterpret_cast<uint64_t>(swapchain));
  if (images == gRT.mSwapchains.end() || images->second.empty()) {
    return {};
  }
  D3D11_TEXTURE2D_DESC desc {};
  images->second.front()->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  winrt::com_ptr<ID3D11Texture2D> staging;
  if (FAILED(gRT.mDevice->CreateTexture2D(&desc, nullptr, staging.put()))) {
    return {};
  }
  gRT.mContext->CopyResource(staging.get(), images->second.front().get());
  D3D11_MAPPED_SUBRESOURCE mapped {};
  if (FAILED(gRT.mContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return {};
  }
  ChannelPeaks peaks {0, 0};
  for (uint32_t y = 0; y < height && y < desc.Height; y += 4) {
    const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
    for (uint32_t x = 0; x < width && x < desc.Width; x += 4) {
      const auto* p = row + x * 4;// B8G8R8A8
      peaks.mRed = std::max<int>(peaks.mRed, p[2]);
      peaks.mGreenOrBlue = std::max<int>({peaks.mGreenOrBlue, p[0], p[1]});
    }
  }
  gRT.mContext->Unmap(staging.get(), 0);
  return peaks;
}

std::optional<SharedAbi::LayerStatus> ReadLayerStatus() {
  SharedMemory::Status status {};
  auto block = SharedMemory::Open(
    SharedObjectName(SharedAbi::BlockObject, SharedAbi::AbiVersion), sizeof(SharedAbi::Block), status);
  if (!block.IsValid()) {
    return std::nullopt;
  }
  SharedAbi::LayerStatus copy {};
  auto& section = block.As<SharedAbi::Block>()->mStatus;
  if (!Seqlock::ReadSection(section, section.mSequence, copy)) {
    return std::nullopt;
  }
  return copy;
}

int CountInLayerLog(std::string_view text) {
  const auto file = Product::LogsFolder() / L"layer-host-layer-test.log";
  const auto contents = FileUtil::ReadAll(file).value_or("");
  int count = 0;
  for (auto at = contents.find(text); at != std::string::npos; at = contents.find(text, at + 1)) {
    ++count;
  }
  return count;
}

}// namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("Usage: host-layer-test <layer DLL> <host exe>\n");
    return 2;
  }
  gHostExe = std::filesystem::absolute(argv[2]).wstring();
  timeBeginPeriod(1);
  const auto instance = std::format(L"hlt{}", GetCurrentProcessId());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", instance.c_str());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_TEST_PANEL", L"0");

  // The fake runtime's device: the default GPU, or WARP without one. The
  // host follows whichever adapter the layer reports.
  for (const auto driver: {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    if (SUCCEEDED(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
          D3D11_SDK_VERSION, gRT.mDevice.put(), nullptr, gRT.mContext.put()))) {
      break;
    }
  }
  Layer layer;
  layer.mModule = LoadLibraryA(argv[1]);
  if (!layer.mModule || XR_FAILED(layer.Negotiate(std::string {Product::LayerName}.c_str()))) {
    std::printf("Couldn't load the layer\n");
    return 1;
  }

  Game game {layer};
  game.CreateSession();

  Scenario("Host panels appear; the animated one at its cap, the static one copied once", [&] {
    HostProcess host {L""};
    Check(host.IsRunning(), "the host started");
    const auto appear = RunFrames(game, 8s, [] { return Quads() == 2; });
    Check(Quads() == 2, std::format("2 panels composed after {} frames", appear.mFrames));
    RunFrames(game, 500ms);// settle: first copies done
    const auto before = gRT.mReleasesBySwapchain;
    const auto run = RunFrames(game, 2s);
    std::vector<int> deltas;
    for (const auto& quad: gRT.mLastQuads) {
      const auto id = reinterpret_cast<uint64_t>(quad.subImage.swapchain);
      const auto previous = before.contains(id) ? before.at(id) : 0;
      deltas.push_back(gRT.mReleasesBySwapchain[id] - previous);
    }
    std::ranges::sort(deltas);
    Check(deltas.size() == 2 && deltas[0] == 0, std::format("the static panel: 0 copies in 2 s ({})", deltas.empty() ? -1 : deltas[0]));
    Check(
      deltas.size() == 2 && deltas[1] >= 50 && deltas[1] <= 66,
      std::format("the animated panel: {} copies in 2 s (cap 30/s, +10% allowed)", deltas.size() == 2 ? deltas[1] : -1));
    Check(run.mMaxEndFrameMs < 5, std::format("xrEndFrame never stalled (max {:.2f} ms)", run.mMaxEndFrameMs));

    host.Kill();
    const auto gone = RunFrames(game, 3s, [] { return Quads() == 0; });
    Check(Quads() == 0, std::format("host killed: panels gone after {} frames", gone.mFrames));
    const auto after = RunFrames(game, 1s);
    Check(after.mMaxEndFrameMs < 5, std::format("no stall while the host is gone (max {:.2f} ms)", after.mMaxEndFrameMs));

    HostProcess restarted {L""};
    const auto back = RunFrames(game, 10s, [] { return Quads() == 2; });
    Check(Quads() == 2, std::format("host restarted: panels back after {} frames", back.mFrames));
    Check(back.mMaxEndFrameMs < 10, std::format("re-attaching stayed cheap (max {:.2f} ms)", back.mMaxEndFrameMs));
  });

  Scenario("An exited host's block, held open by another viewer, is never connected to (test round 5)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L""};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    // Another viewer (the mirror, in test round 5) keeps the block alive
    SharedMemory::Status status {};
    auto viewer = SharedMemory::Open(
      SharedObjectName(SharedAbi::BlockObject, SharedAbi::AbiVersion), sizeof(SharedAbi::Block), status);
    Check(viewer.IsValid(), "a second viewer has the block open");
    std::this_thread::sleep_for(400ms);// the log is written in the background
    const auto connects = CountInLayerLog("connected to the host app");
    const auto failedOpens = CountInLayerLog("couldn't open panel");
    host.Kill();
    RunFrames(game, 3s, [] { return Quads() == 0; });
    const auto run = RunFrames(game, 3500ms);// reconnects are tried every second
    std::this_thread::sleep_for(400ms);
    Check(Quads() == 0, "no panels");
    Check(CountInLayerLog("connected to the host app") == connects, "no reconnect to the exited host");
    Check(CountInLayerLog("couldn't open panel") == failedOpens, "no failed texture opens");
    Check(run.mMaxEndFrameMs < 5, std::format("no stall (max {:.2f} ms)", run.mMaxEndFrameMs));
  });

  Scenario("600 changes to one panel, as a slider makes them: the other keeps its image (doc 11, RN-10)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L"--static-panels"};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    RunFrames(game, 1s);// first copies done
    // The first panel in the list is on the left; the other on the right
    uint64_t left = 0;
    uint64_t right = 0;
    float leftX = 0;
    for (const auto& quad: gRT.mLastQuads) {
      const auto id = reinterpret_cast<uint64_t>(quad.subImage.swapchain);
      if (quad.pose.position.x < 0) {
        left = id;
        leftX = quad.pose.position.x;
      } else {
        right = id;
      }
    }
    const auto before = gRT.mReleasesBySwapchain;
    const auto created = gRT.mSwapchainsCreated;
    // 600 nudges of 1 mm to the first panel, through the command window
    // (M6e), while the game keeps rendering
    auto className = SharedObjectName(L"Automation", SharedAbi::AbiVersion);
    className.erase(0, className.find(L'\\') + 1);
    HWND commands = nullptr;
    RunFrames(game, 5s, [&] { return (commands = FindWindowExW(HWND_MESSAGE, nullptr, className.c_str(), nullptr)) != nullptr; });
    std::atomic<int> done {0};
    std::atomic<int> answered {0};
    std::thread sender([&] {
      const auto words = Automation::Pack(std::vector<std::wstring> {L"nudge", L"#1", L"right", L"0.1"});
      for (int i = 0; i < 600 && commands; ++i) {
        COPYDATASTRUCT copy {Automation::WireTag, static_cast<DWORD>(words.size() * sizeof(wchar_t)), const_cast<wchar_t*>(words.data())};
        DWORD_PTR result = 0;
        SendMessageTimeoutW(commands, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy), SMTO_ABORTIFHUNG, 3000, &result);
        done += result == 100 ? 1 : 0;
        ++answered;
      }
    });
    const auto run = RunFrames(game, 30s, [&] { return answered.load() >= 600 || !commands; });
    sender.join();
    RunFrames(game, 1500ms);// the last ones run and are shown
    Check(commands != nullptr && done >= 500, std::format("{} of 600 changes taken", done.load()));
    float movedTo = leftX;
    for (const auto& quad: gRT.mLastQuads) {
      if (reinterpret_cast<uint64_t>(quad.subImage.swapchain) == left) {
        movedTo = quad.pose.position.x;
      }
    }
    Check(Quads() == 2, "both panels still showing");
    Check(std::abs((movedTo - leftX) - (done * 0.001f)) < 0.003f,
      std::format("the first panel moved by every change: {:.1f} cm ({:.1f} expected)", (movedTo - leftX) * 100, done * 0.1));
    const auto copied = [&](uint64_t id) { return gRT.mReleasesBySwapchain[id] - (before.contains(id) ? before.at(id) : 0); };
    Check(right != 0 && copied(right) == 0, std::format("the other panel: not copied again, its image kept ({} copies)", copied(right)));
    Check(gRT.mSwapchainsCreated == created, "no swapchain made again for either");
    Check(run.mMaxEndFrameMs < 5, std::format("xrEndFrame never stalled meanwhile (max {:.2f} ms)", run.mMaxEndFrameMs));
  });

  Scenario("Panels resized every frame keep their swapchains (RN-7)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L"--resizing-panels"};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    RunFrames(game, 1s);// first frames, and the textures' first growth
    const auto created = gRT.mSwapchainsCreated;
    std::set<int32_t> widths;
    bool withinImages = true;
    const auto run = RunFrames(game, 2s, [&] {
      for (const auto& quad: gRT.mLastQuads) {
        const auto& rect = quad.subImage.imageRect;
        widths.insert(rect.extent.width);
        const auto images = gRT.mSwapchains.find(reinterpret_cast<uint64_t>(quad.subImage.swapchain));
        if (images != gRT.mSwapchains.end() && !images->second.empty()) {
          D3D11_TEXTURE2D_DESC desc {};
          images->second.front()->GetDesc(&desc);
          withinImages &= rect.offset.x == 0 && rect.offset.y == 0 && rect.extent.width > 0
            && static_cast<UINT>(rect.extent.width) <= desc.Width && static_cast<UINT>(rect.extent.height) <= desc.Height;
        }
      }
      return false;
    });
    Check(Quads() == 2, "both panels showing");
    Check(widths.size() >= 3, std::format("the frames had different sizes ({} widths)", widths.size()));
    Check(gRT.mSwapchainsCreated == created,
      std::format("no swapchain created while they changed size ({})", gRT.mSwapchainsCreated - created));
    Check(withinImages, "each quad shows the top-left part of its image, its frame's size");
    Check(run.mMaxEndFrameMs < 5, std::format("no stall (max {:.2f} ms)", run.mMaxEndFrameMs));
  });

  Scenario("Gaze focus: the layer reports the panel the head points at (PN-4, doc 05)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L"--static-panels"};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    // The animated test panel is at (-0.22, -0.12, -0.55): turn and tilt
    // the head toward its centre, from the origin
    const float yaw = std::atan2(0.22f, 0.55f);
    const float pitch = -std::atan2(0.12f, std::sqrt(0.22f * 0.22f + 0.55f * 0.55f));
    const XrQuaternionf turn {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    const XrQuaternionf tilt {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    const auto focused = [] {
      const auto status = ReadLayerStatus();
      return status ? status->mFocusedPanelID : 0;
    };
    // Before: the head looks far above the panels
    gRT.mHeadPose = {{0, 0, 0, 1}, {0, 1.2f, 0}};
    RunFrames(game, 300ms);
    Check(focused() == 0, "looking elsewhere: nothing focused");
    gRT.mHeadPose = {{turn.w * tilt.x, turn.y * tilt.w, -turn.y * tilt.x, turn.w * tilt.w}, {0, 0, 0}};
    const auto start = TimeUtil::Now();
    RunFrames(game, 3s, [&] { return focused() == 0x0D0E'0001; });
    Check(focused() == 0x0D0E'0001,
      std::format("looking at the left panel: it's focused ({:.0f} ms)", TimeUtil::ToMilliseconds(TimeUtil::Now() - start)));
    gRT.mHeadPose = {{0, 0, 0, 1}, {0, 0, 0}};// straight ahead: between the two panels
    RunFrames(game, 3s, [&] { return focused() == 0; });
    Check(focused() == 0, "looking between them: nothing focused");
    gRT.mHeadPose = {{0, 0, 0, 1}, {0, 1.2f, 0}};
  });

  Scenario("A panel tinted red at half brightness: no green or blue left in the game's image (PN-5)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L"--static-panels --test-look"};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    RunFrames(game, 500ms);
    std::vector<ChannelPeaks> peaks;
    for (const auto& quad: gRT.mLastQuads) {
      peaks.push_back(Peaks(quad.subImage.swapchain, quad.subImage.imageRect.extent.width, quad.subImage.imageRect.extent.height));
    }
    // Which is which: the tinted one is the one without green or blue
    std::ranges::sort(peaks, {}, &ChannelPeaks::mGreenOrBlue);
    Check(peaks.size() == 2, "both panels' images read");
    if (peaks.size() == 2) {
      std::printf("  (tinted: red up to %d, green/blue up to %d; plain: red up to %d, green/blue up to %d)\n", peaks[0].mRed,
        peaks[0].mGreenOrBlue, peaks[1].mRed, peaks[1].mGreenOrBlue);
      Check(peaks[0].mGreenOrBlue <= 2, "the tinted panel: no green or blue");
      Check(peaks[0].mRed > 40 && peaks[0].mRed < 200, "its red dimmed, not gone");
      Check(peaks[1].mGreenOrBlue > 100, "the other panel is untouched");
    }
  });

  Scenario("A keybind hides and shows all panels in the game: Raw Input to the layer (PN-6, KY-1, KY-3)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {std::format(L"--static-panels --test-keybind {}", VK_F24)};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    Check(Quads() == 2, "both panels showing");
    RunFrames(game, 500ms);// the host's input thread is watching by now
    const auto press = [] {
      INPUT keys[2] {};
      keys[0].type = keys[1].type = INPUT_KEYBOARD;
      keys[0].ki.wVk = keys[1].ki.wVk = VK_F24;
      keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(2, keys, sizeof(INPUT));
    };
    press();
    const auto hidden = RunFrames(game, 3s, [] { return Quads() == 0; });
    Check(Quads() == 0, std::format("F24 (sent from outside): all hidden after {} frames", hidden.mFrames));
    press();
    const auto shown = RunFrames(game, 3s, [] { return Quads() == 2; });
    Check(Quads() == 2, std::format("F24 again: all back after {} frames (the captures were kept)", shown.mFrames));
    Check(hidden.mMaxEndFrameMs < 5 && shown.mMaxEndFrameMs < 5, "no stall");
  });

  Scenario("A version mismatch passes through, with one log line (SY-7)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });// the previous host is gone
    SharedMemory::Status status {};
    auto fake = SharedMemory::Create(
      SharedObjectName(SharedAbi::BlockObject, SharedAbi::AbiVersion), sizeof(SharedAbi::Block), status);
    Check(fake.IsValid(), std::format("a fake host block with another ABI exists ({})", SharedMemory::StatusName(status)));
    if (!fake.IsValid()) {
      return;
    }
    auto& header = fake.As<SharedAbi::Block>()->mHeader;
    header.mAbiVersion = SharedAbi::AbiVersion + 1;
    header.mBlockSize = sizeof(SharedAbi::Block);
    header.mHostProcessID = GetCurrentProcessId();
    header.mMagic = SharedAbi::Magic;
    const auto linesBefore = CountInLayerLog("different version");
    const auto run = RunFrames(game, 6500ms);
    std::this_thread::sleep_for(400ms);// the log is written in the background
    Check(Quads() == 0, "no panels from a mismatched host");
    Check(CountInLayerLog("different version") - linesBefore == 1, "logged once, though checked again 5 s later");
    Check(run.mMaxEndFrameMs < 5, std::format("no stall (max {:.2f} ms)", run.mMaxEndFrameMs));
  });

  Scenario("A host whose GPU work never completes never gets a GPU wait (ADR-024)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L"--fence-delay-ms 600000"};
    const auto before = ReadLayerStatus();
    const auto run = RunFrames(game, 4s);
    std::this_thread::sleep_for(1200ms);
    RunFrames(game, 100ms);
    const auto after = ReadLayerStatus();
    Check(Quads() == 0, "nothing is shown until a frame is ready");
    Check(after.has_value(), "the layer is connected and reporting");
    const auto waits = after ? after->mForcedWaits - (before ? before->mForcedWaits : 0) : 1;
    Check(waits == 0, std::format("no GPU wait on a fence that never signals ({})", waits));
    Check(run.mMaxEndFrameMs < 5, std::format("no stall (max {:.2f} ms)", run.mMaxEndFrameMs));
    host.Kill();
  });

  Scenario("The host's fence 100 ms late: keep previous, rare GPU waits, no CPU wait (ADR-012)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L"--fence-delay-ms 100"};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    RunFrames(game, 1s);
    const auto before = ReadLayerStatus();
    const auto run = RunFrames(game, 3s);
    std::this_thread::sleep_for(1200ms);// the layer reports once a second
    RunFrames(game, 100ms);
    const auto after = ReadLayerStatus();
    Check(before && after, "the layer's report is readable");
    if (!before || !after) {
      return;
    }
    const auto kept = after->mKeptPrevious - before->mKeptPrevious;
    const auto waits = after->mForcedWaits - before->mForcedWaits;
    const auto frames = static_cast<uint64_t>(run.mFrames);
    std::printf("  (%d frames: %llu kept the previous image, %llu forced GPU waits)\n", run.mFrames,
      static_cast<unsigned long long>(kept), static_cast<unsigned long long>(waits));
    Check(kept > 0, "late frames kept the previous image");
    Check(waits > 0 && waits * 2 < frames, "forced waits happened, on a minority of frames");
    Check(run.mMaxEndFrameMs < 5, std::format("the game's CPU never waited (max {:.2f} ms)", run.mMaxEndFrameMs));
  });

  Scenario("Static panels: nothing is copied while nothing changes (idle bench gate)", [&] {
    RunFrames(game, 3s, [] { return Quads() == 0; });
    HostProcess host {L"--static-panels"};
    RunFrames(game, 10s, [] { return Quads() == 2; });
    RunFrames(game, 500ms);
    const auto releases = gRT.mReleases;
    const auto run = RunFrames(game, 2s);
    Check(Quads() == 2, "both panels showing");
    Check(gRT.mReleases == releases, std::format("0 images copied in {} frames", run.mFrames));
  });

  timeEndPeriod(1);
  return Finish();
}
