// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The shared-memory block between the host app and the layer (doc 02,
// "Shared state uses fixed layouts, versions and seqlocks"). Our own design.
//
// Rules (SY-6 to SY-9):
// - Fixed-size types only, explicit padding, a static_assert on every size.
//   Any layout change bumps AbiVersion.
// - The host creates the block; layers only open it. The kernel object name
//   carries the ABI version, and the header repeats it with the sizes, so a
//   mismatched layer passes through with one log line.
// - Each section has a sequence counter (seqlock). Writers make it odd while
//   writing; readers copy, then check it didn't change.
// - Handles are values in the host process; the layer duplicates them into
//   itself (it pulls; the host never opens the game).
#pragma once

#include <cstddef>
#include <cstdint>

namespace OpenDesktopVR::SharedAbi {

inline constexpr uint32_t Magic = 0x5244444F;// "ODDR" little-endian: Open Desktop, data region
inline constexpr uint32_t AbiVersion = 1;
inline constexpr uint32_t MaxPanels = 16;
inline constexpr uint32_t RingSlots = 3;

// Kernel object names (with Product::KernelObjectName and AbiVersion)
inline constexpr wchar_t BlockObject[] = L"Shared";

struct Header {
  uint32_t mMagic;
  uint32_t mAbiVersion;
  uint32_t mBlockSize;// sizeof(Block)
  uint32_t mHostProcessID;
  // Bumped whenever the host re-creates shared resources (device change,
  // restart of rendering). Layers drop and re-open everything on a change.
  uint64_t mSessionGeneration;
  uint32_t mPanelEntrySize;// sizeof(PanelEntry)
  uint32_t mStatusSize;    // sizeof(LayerStatus)
  uint8_t mReserved[32];
};
static_assert(sizeof(Header) == 64);

namespace PanelFlags {
inline constexpr uint32_t Visible = 1u << 0;
inline constexpr uint32_t Focused = 1u << 1;
}// namespace PanelFlags

struct PanelEntry {
  uint64_t mPanelID;// stable for the panel's lifetime
  uint32_t mFlags;  // PanelFlags
  uint32_t mCurrentSlot;// newest published ring slot, 0..RingSlots-1
  // Increments with every published frame; 0 = nothing published yet
  uint64_t mContentVersion;
  // The fence value that marks mCurrentSlot's frame complete
  uint64_t mFenceValue;
  // Pose in LOCAL space: position (x, y, z), orientation (x, y, z, w)
  float mPose[7];
  float mSizeMeters[2];// width, height
  uint32_t mTextureWidth; // ring texture size
  uint32_t mTextureHeight;
  uint32_t mContentWidth; // the part of the texture holding content (top-left)
  uint32_t mContentHeight;
  // Bumped when the ring's textures are re-created; layers re-open them
  uint32_t mTextureGeneration;
  // DXGI_FORMAT of the ring textures (B8G8R8A8_UNORM or R8G8B8A8_UNORM).
  // The host matches the layer's preferred family, so the game process only
  // ever copies, never converts.
  uint32_t mTextureFormat;
  uint32_t mPad0;// explicit: the handles below start on 8 bytes
  // NT handle values, valid in the host process
  uint64_t mTextureHandles[RingSlots];
  uint64_t mFenceHandle;
  uint8_t mReserved[64];
};
static_assert(sizeof(PanelEntry) == 192);
static_assert(offsetof(PanelEntry, mTextureHandles) == 96);

struct PanelTable {
  uint64_t mSequence;// seqlock
  uint32_t mPanelCount;
  uint32_t mPad0;
  PanelEntry mPanels[MaxPanels];
};
static_assert(sizeof(PanelTable) == 16 + MaxPanels * sizeof(PanelEntry));

// Written by the layer that owns it (the first to claim it), read by the
// host: what the host needs to know about the game (doc 03 "Layer to host")
struct LayerStatus {
  uint64_t mSequence;// seqlock
  uint32_t mLayerProcessID;// 0 = unclaimed
  uint32_t mGraphicsApi;   // GraphicsApi
  uint32_t mAdapterLuidLow;// the game device's adapter: the host renders on it
  int32_t mAdapterLuidHigh;
  uint32_t mSessionState;  // XrSessionState
  uint32_t mMaxLayerCount;
  uint64_t mFramesSinceAttach;
  uint64_t mPanelsCopied;  // panel images copied into swapchains (copy counter)
  uint64_t mKeptPrevious;  // frames that kept a previous image (ADR-012)
  uint64_t mForcedWaits;   // GPU waits after 40 ms
  int32_t mLastError;      // XrResult, or 0
  uint32_t mHandTrackingAvailable;
  char mExecutable[128];   // UTF-8 file name, zero-terminated
  char mApplication[128];  // XrApplicationInfo::applicationName
  char mRuntime[128];      // XrInstanceProperties::runtimeName
  // DXGI_FORMAT of the layer's panel swapchains: the host draws panels in
  // the same channel order
  uint32_t mPreferredPanelFormat;
  uint32_t mPad0;
  uint8_t mReserved[48];
};
static_assert(sizeof(LayerStatus) == 512);

enum class GraphicsApi : uint32_t { Unknown = 0, D3D11 = 1, D3D12 = 2, Vulkan = 3, OpenGL = 4 };

struct Block {
  Header mHeader;
  PanelTable mPanelTable;
  LayerStatus mStatus;
};
static_assert(sizeof(Block) == 64 + sizeof(PanelTable) + 512);
static_assert(alignof(Block) == 8);

// The header a host writes; the fields a layer checks
constexpr bool IsCompatible(const Header& header) noexcept {
  return header.mMagic == Magic && header.mAbiVersion == AbiVersion && header.mBlockSize == sizeof(Block)
    && header.mPanelEntrySize == sizeof(PanelEntry) && header.mStatusSize == sizeof(LayerStatus);
}

}// namespace OpenDesktopVR::SharedAbi
