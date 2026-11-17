// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Other products' OpenXR API layers whose order matters to ours (doc 03
// "Layer order", doc 10; CMP-1, CMP-2):
// - Ultraleap's hand-tracking layer must be last, nearest the runtime: it
//   doesn't pass calls on to layers below it *(reported)*;
// - pose-changing layers (neck safety, motion compensation) belong below
//   ours, so panels follow the adjusted world *(reported)*;
// - mirror and capture layers belong below ours if they should record the
//   panels.
//
// Known by words in their layer name or manifest path, case-insensitively.
// The words are unverified against each product's current release; a miss
// only means no advice is shown. Used by the layer (its report to the host)
// and the host (the self-test), so both agree.
#pragma once

#include <cstdint>
#include <string_view>

namespace OpenDesktopVR::KnownLayers {

// Bits, so a set of them fits one field of SharedAbi::LayerStatus
enum Kind : uint32_t {
  None = 0,
  Ultraleap = 1u << 0,
  PoseChanging = 1u << 1,
  MirrorCapture = 1u << 2,
};

Kind Classify(std::string_view nameOrPath) noexcept;

// "Ultraleap hand tracking", "a pose-changing layer", ...
std::string_view Describe(Kind kind) noexcept;

}// namespace OpenDesktopVR::KnownLayers
