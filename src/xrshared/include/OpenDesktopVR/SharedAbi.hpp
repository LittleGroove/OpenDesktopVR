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
// 2: the header's viewer heartbeat
// 3: each ring slot's content size; ring textures only grow (RN-7)
// 4: the host's wake event; the panel the head points at (gaze focus)
// 5: hands: the tracking ring (layer to host), the hand state (host to
//    layer) and the host's tracking event
inline constexpr uint32_t AbiVersion = 5;
inline constexpr uint32_t MaxPanels = 16;
inline constexpr uint32_t RingSlots = 3;
inline constexpr uint32_t TrackingSlots = 8;
inline constexpr uint32_t HandJointCount = 26;// XR_HAND_JOINT_SET_DEFAULT_EXT

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
  // GetTickCount64() of the last time any viewer (a layer, the mirror) was
  // looking. The host draws nothing while nobody watches.
  uint64_t mViewerHeartbeat;
  // An auto-reset event (a handle value in the host process) the layer
  // sets when its report changed something the host acts on at once: the
  // focused panel. The layer pulls it, like the textures (SY-9).
  uint64_t mWakeEvent;
  // An auto-reset event (a handle value in the host process) the layer
  // sets after writing a tracking sample; the host's hand thread waits on it
  uint64_t mTrackingEvent;
  // Bumped by the host's "Recenter" keybind: layers put panels where the
  // head is then (ADR-037). These 8 bytes were reserved: an older host
  // leaves them 0, which never asks, so the version stays the same.
  uint64_t mRecenterCount;
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
  float mSizeMeters[2];// width, height (the newest frame's aspect)
  // Ring texture size. Textures only grow (RN-7): a smaller frame uses the
  // top-left part, so resizing and trimming never re-create the textures or
  // the layer's swapchain.
  uint32_t mTextureWidth;
  uint32_t mTextureHeight;
  uint32_t mContentWidth; // the newest frame's part of the texture (top-left)
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
  // Each slot's content size (width, height): a slot shown late can hold a
  // frame of another size than the newest. 0 x 0 for a slot not drawn yet.
  uint32_t mSlotContentSize[RingSlots][2];
  uint8_t mReserved[64 - RingSlots * 8];
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
  // The panel the head points at (doc 05 "Gaze focus"), or 0: the host
  // draws it with its "looked at" opacity (PN-4)
  uint64_t mFocusedPanelID;
  // Diagnostics (M6c; CMP-1, CMP-2, CMP-7). Formerly reserved: a layer
  // from before M6c leaves them zero, which reads as "not known".
  uint32_t mChainFlags;    // ChainFlags
  uint32_t mLayersBelow;   // API layers between ours and the runtime
  uint32_t mKnownBelow;    // KnownLayers::Kind bits among them
  uint32_t mNearestRuntime;// the Kind of the one nearest the runtime, or 0
  uint8_t mReserved[24];
};
static_assert(sizeof(LayerStatus) == 512);
static_assert(offsetof(LayerStatus, mChainFlags) == 472);

enum ChainFlags : uint32_t {
  ChainKnown = 1u << 0,  // the fields above are filled in
  DepthEnabled = 1u << 1,// the game enabled XR_KHR_composition_layer_depth
};

enum class GraphicsApi : uint32_t { Unknown = 0, D3D11 = 1, D3D12 = 2, Vulkan = 3, OpenGL = 4 };

// One hand joint, in the layer's LOCAL space
struct JointPose {
  float mPose[7];// position (x, y, z), orientation (x, y, z, w)
  float mRadius; // meters
};
static_assert(sizeof(JointPose) == 32);

struct HandSample {
  // 1 when every joint was valid and finite this frame (XR-10); otherwise
  // the joints are zero and the hand is untracked
  uint32_t mTracked;
  uint32_t mPad0;
  JointPose mJoints[HandJointCount];// XrHandJointEXT order
};
static_assert(sizeof(HandSample) == 8 + HandJointCount * sizeof(JointPose));

namespace SampleFlags {
inline constexpr uint32_t HeadValid = 1u << 0;
}// namespace SampleFlags

// What the layer saw in one frame (doc 03 "Layer to host"). Written only
// while panels are composed: no samples for 250 ms means "absent" (XR-19).
struct TrackingSample {
  uint64_t mSequence;// seqlock, per slot
  uint64_t mIndex;   // which sample this slot holds: 1, 2, ...
  int64_t mDisplayTime;// XrTime the joints were located at (nanoseconds)
  int64_t mWrittenAt;  // QueryPerformanceCounter when written: one clock for every process
  uint32_t mFlags;     // SampleFlags
  uint32_t mPad0;
  float mHead[7];// the head (VIEW space) in LOCAL: position, orientation
  float mPad1;
  HandSample mHands[2];// left, right
};
static_assert(sizeof(TrackingSample) == 72 + 2 * sizeof(HandSample));

// Written by the layer that owns the status section, read by the host: the
// newest sample is mSlots[mWritten % TrackingSlots]. A reader that falls
// more than TrackingSlots behind skips the oldest; a slot whose mIndex
// isn't the one expected was overwritten and is skipped too.
struct TrackingRing {
  uint64_t mWritten;// samples written so far (atomic)
  uint8_t mReserved[56];
  TrackingSample mSlots[TrackingSlots];
};
static_assert(sizeof(TrackingRing) == 64 + TrackingSlots * sizeof(TrackingSample));

namespace HandFlags {
// The host wants tracking samples (hands are on in its settings)
inline constexpr uint32_t WantsSamples = 1u << 0;
}// namespace HandFlags

// Written by the host's hand thread, read by the layer (doc 03 "Host to
// layer"): how much of each hand to draw. The host decides (the pointing
// pose, settings); the layer draws in the same frame it tracks.
struct HandState {
  uint64_t mSequence;// seqlock
  float mVisibility[2];// left, right: 0 = hidden, 1 = fully shown
  uint32_t mFlags;     // HandFlags
  uint32_t mPad0;
  uint64_t mAnsweredSample;// the sample these values came from
  uint8_t mReserved[32];
};
static_assert(sizeof(HandState) == 64);

struct Block {
  Header mHeader;
  PanelTable mPanelTable;
  LayerStatus mStatus;
  HandState mHands;
  TrackingRing mTracking;
};
static_assert(sizeof(Block) == 64 + sizeof(PanelTable) + 512 + 64 + sizeof(TrackingRing));
static_assert(alignof(Block) == 8);

// The header a host writes; the fields a layer checks
constexpr bool IsCompatible(const Header& header) noexcept {
  return header.mMagic == Magic && header.mAbiVersion == AbiVersion && header.mBlockSize == sizeof(Block)
    && header.mPanelEntrySize == sizeof(PanelEntry) && header.mStatusSize == sizeof(LayerStatus);
}

}// namespace OpenDesktopVR::SharedAbi
