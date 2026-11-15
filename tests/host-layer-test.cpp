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
// - static panels: nothing copied while nothing changes (bench gate).

#include "check.hpp"
#include "fake-openxr.hpp"

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/Seqlock.hpp>
#include <OpenDesktopVR/SharedAbi.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <timeapi.h>

#include <chrono>
#include <format>
#include <functional>
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

  Scenario("The host's fence 100 ms late: keep previous, rare GPU waits, no CPU wait (ADR-012)", [&] {
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
