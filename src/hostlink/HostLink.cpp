// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/HostLink.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/Seqlock.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace OpenDesktopVR::HostLink {

using namespace std::chrono_literals;

namespace {

bool ProcessHasExited(uint32_t processID) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processID);
  if (!process) {
    return GetLastError() == ERROR_INVALID_PARAMETER;// no such process
  }
  const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
  CloseHandle(process);
  return exited;
}

}// namespace

Client::Client(ID3D11Device* device, ClientOptions options) : mOptions(options) {
  if (device) {
    device->QueryInterface(mDevice1.put());
    device->QueryInterface(mDevice5.put());
  }
  if (!mDevice5) {
    Log::Warning("Host link: this device has no shared fences (Windows 10 1703 or later is needed); no host panels");
  }
}

Client::~Client() {
  Disconnect(nullptr);
}

void Client::DropResources() noexcept {
  for (auto& resources: mResources) {
    resources = {};
  }
}

void Client::Disconnect(const char* why) noexcept {
  if (mWakeEvent) {
    CloseHandle(mWakeEvent);
    mWakeEvent = nullptr;
  }
  if (mHostProcess) {
    CloseHandle(mHostProcess);
    mHostProcess = nullptr;
    ++mCounters.mDisconnects;
    if (why) {
      Log::Info("Host link: {}; panels hidden until it's back", why);
    }
  }
  DropResources();
  mSnapshot.mPanelCount = 0;
  mMemory = {};
  mHostProcessID = 0;
  mOwnsStatus = false;
}

void Client::TryConnect() noexcept {
  SharedMemory::Status status {};
  auto memory = SharedMemory::Open(
    SharedObjectName(SharedAbi::BlockObject, SharedAbi::AbiVersion), sizeof(SharedAbi::Block), status);
  if (status != mLastOpenStatus && status != SharedMemory::Status::Ok && status != SharedMemory::Status::NotFound) {
    Log::Warning("Host link: can't open the shared block ({})", SharedMemory::StatusName(status));
  }
  mLastOpenStatus = status;
  if (!memory.IsValid()) {
    return;
  }
  const auto& header = memory.As<SharedAbi::Block>()->mHeader;
  if (std::atomic_ref<uint32_t> {const_cast<uint32_t&>(header.mMagic)}.load(std::memory_order_acquire) != SharedAbi::Magic) {
    return;// the host is still setting it up
  }
  if (!SharedAbi::IsCompatible(header)) {
    if (!mLoggedMismatch) {
      mLoggedMismatch = true;
      Log::Warning(
        "Host link: the host app is a different version (ABI {}, block {} bytes; this layer: ABI {}, {} bytes). "
        "Passing through until they match",
        header.mAbiVersion,
        header.mBlockSize,
        SharedAbi::AbiVersion,
        sizeof(SharedAbi::Block));
    }
    mNextAttempt = TimeUtil::Now() + mOptions.mMismatchRetryInterval;
    return;
  }
  // The layer pulls handles out of the host (SY-9)
  HANDLE process = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, header.mHostProcessID);
  if (!process) {
    Log::Warning("Host link: can't open the host process ({}); is one of them elevated?", GetLastError());
    return;
  }
  if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
    // The block of a host that has exited, kept alive by another viewer that
    // still has it open: not a host. Connecting to it failed to open every
    // texture, once a second, until that viewer let go (test round 5).
    CloseHandle(process);
    return;
  }
  // The host's wake event, pulled like the textures (SY-9); optional
  HANDLE wake = nullptr;
  if (header.mWakeEvent != 0
      && !DuplicateHandle(process, reinterpret_cast<HANDLE>(header.mWakeEvent), GetCurrentProcess(), &wake, EVENT_MODIFY_STATE, FALSE, 0)) {
    wake = nullptr;
  }
  mMemory = std::move(memory);
  mHostProcess = process;
  mWakeEvent = wake;
  mHostProcessID = header.mHostProcessID;
  mGeneration = std::atomic_ref<uint64_t> {const_cast<uint64_t&>(header.mSessionGeneration)}.load();
  mSnapshot.mPanelCount = 0;
  ++mCounters.mConnects;
  Log::Info("Host link: connected to the host app (process {})", mHostProcessID);
}

void Client::WakeHost() noexcept {
  if (mWakeEvent) {
    SetEvent(mWakeEvent);
  }
}

bool Client::HostIsRunning() const noexcept {
  return mHostProcess && WaitForSingleObject(mHostProcess, 0) == WAIT_TIMEOUT;
}

void Client::Update(TimeUtil::TimePoint now) noexcept {
  if (!mDevice5) {
    return;
  }
  if (!IsConnected()) {
    if (now >= mNextAttempt) {
      mNextAttempt = now + mOptions.mReconnectInterval;
      TryConnect();
    }
    if (!IsConnected()) {
      return;
    }
  }
  if (now >= mNextHostCheck) {
    mNextHostCheck = now + mOptions.mHostCheckInterval;
    if (WaitForSingleObject(mHostProcess, 0) == WAIT_OBJECT_0) {
      Disconnect("the host app exited");
      return;
    }
  }
  if (now >= mNextHeartbeat) {
    mNextHeartbeat = now + mOptions.mHostCheckInterval;
    std::atomic_ref<uint64_t> {Block()->mHeader.mViewerHeartbeat}.store(GetTickCount64(), std::memory_order_relaxed);
  }
  const auto generation = std::atomic_ref<uint64_t> {Block()->mHeader.mSessionGeneration}.load();
  if (generation != mGeneration) {
    mGeneration = generation;
    DropResources();
  }
  // A clash with the host's write keeps the previous snapshot (SY-6)
  Seqlock::ReadSection(Block()->mPanelTable, Block()->mPanelTable.mSequence, mSnapshot);
  mSnapshot.mPanelCount = std::min(mSnapshot.mPanelCount, SharedAbi::MaxPanels);// untrusted (TH-9)
}

bool Client::Open(PanelResources& resources, const SharedAbi::PanelEntry& entry) noexcept {
  resources = {};
  resources.mPanelID = entry.mPanelID;
  resources.mGeneration = mGeneration;
  resources.mTextureGeneration = entry.mTextureGeneration;
  ++mCounters.mResourceOpens;
  const auto pull = [&](uint64_t value) -> HANDLE {
    HANDLE local = nullptr;
    if (!value || !DuplicateHandle(mHostProcess, reinterpret_cast<HANDLE>(value), GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      return nullptr;
    }
    return local;
  };
  for (uint32_t slot = 0; slot < SharedAbi::RingSlots; ++slot) {
    HANDLE local = pull(entry.mTextureHandles[slot]);
    const bool opened = local
      && SUCCEEDED(mDevice1->OpenSharedResource1(local, __uuidof(ID3D11Texture2D), resources.mTextures[slot].put_void()));
    if (local) {
      CloseHandle(local);
    }
    if (!opened) {
      resources.mFailed = true;
    }
  }
  if (HANDLE local = pull(entry.mFenceHandle)) {
    if (FAILED(mDevice5->OpenSharedFence(local, __uuidof(ID3D11Fence), resources.mFence.put_void()))) {
      resources.mFailed = true;
    }
    CloseHandle(local);
  } else {
    resources.mFailed = true;
  }
  if (resources.mFailed) {
    ++mCounters.mOpenFailures;
    Log::Warning(
      "Host link: couldn't open panel {:X}'s textures (different GPU, or the host re-created them)", entry.mPanelID);
    resources.mTextures = {};
    resources.mFence = nullptr;
  }
  return !resources.mFailed;
}

const PanelResources* Client::Resources(const SharedAbi::PanelEntry& entry, bool& openBudget) noexcept {
  if (!IsConnected() || entry.mContentVersion == 0) {
    return nullptr;
  }
  PanelResources* slot = nullptr;
  for (auto& resources: mResources) {
    if (resources.mPanelID == entry.mPanelID) {
      slot = &resources;
      break;
    }
  }
  if (slot && slot->mGeneration == mGeneration && slot->mTextureGeneration == entry.mTextureGeneration) {
    return slot->mFailed ? nullptr : slot;
  }
  if (!slot) {
    // Reuse the entry of a panel that's no longer in the table
    for (auto& resources: mResources) {
      const auto panels = Panels();
      const bool listed = std::any_of(panels.begin(), panels.end(), [&](const auto& p) { return p.mPanelID == resources.mPanelID; });
      if (resources.mPanelID == 0 || !listed) {
        slot = &resources;
        break;
      }
    }
  }
  if (!slot || !openBudget) {
    return nullptr;// not this frame
  }
  openBudget = false;
  return Open(*slot, entry) ? slot : nullptr;
}

void Client::WriteStatus(const SharedAbi::LayerStatus& status) noexcept {
  if (!IsConnected() || !mOptions.mWritesStatus) {
    return;
  }
  auto& section = Block()->mStatus;
  std::atomic_ref<uint32_t> owner {section.mLayerProcessID};
  const auto self = GetCurrentProcessId();
  if (!mOwnsStatus) {
    uint32_t expected = 0;
    if (owner.compare_exchange_strong(expected, self)) {
      mOwnsStatus = true;
    } else if (expected != self) {
      // Another game's layer owns it; take over only once that game has exited
      const auto now = TimeUtil::Now();
      if (now < mNextClaimCheck) {
        return;
      }
      mNextClaimCheck = now + 5s;
      if (!ProcessHasExited(expected) || !owner.compare_exchange_strong(expected, self)) {
        return;
      }
      mOwnsStatus = true;
    } else {
      mOwnsStatus = true;
    }
  }
  Seqlock::WriteSection(section.mSequence, [&] {
    auto copy = status;
    copy.mSequence = section.mSequence;
    copy.mLayerProcessID = self;
    std::memcpy(&section, &copy, sizeof(section));
  });
}

}// namespace OpenDesktopVR::HostLink
