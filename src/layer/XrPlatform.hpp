// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// OpenXR headers with the Windows and D3D11 platform types, in the one order
// that works. Include this instead of <openxr/...> inside the layer.
#pragma once

#include <Windows.h>

#include <d3d11_1.h>
#include <unknwn.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <loader_interfaces.h>

#include <type_traits>
