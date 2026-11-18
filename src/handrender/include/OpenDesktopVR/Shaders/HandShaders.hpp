// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Compiled bytecode for shaders/Hand.hlsl. The included headers are generated
// at build time by fxc (see CMakeLists.txt); each defines one `g_*` array.
#pragma once

#include <Windows.h>

#include <span>

namespace OpenDesktopVR::Shaders::Hand::Generated {
#include <HandConeVS.h>
#include <HandPS.h>
#include <HandSphereVS.h>
}// namespace OpenDesktopVR::Shaders::Hand::Generated

namespace OpenDesktopVR::Shaders::Hand {

inline constexpr std::span<const BYTE> ConeVS {Generated::g_HandConeVS};
inline constexpr std::span<const BYTE> SphereVS {Generated::g_HandSphereVS};
inline constexpr std::span<const BYTE> PS {Generated::g_HandPS};

}// namespace OpenDesktopVR::Shaders::Hand
