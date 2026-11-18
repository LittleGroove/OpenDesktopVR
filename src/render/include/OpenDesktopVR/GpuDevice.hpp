// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The host's GPU: one D3D11 device with Direct2D and DirectWrite on top
// (doc 08 "Host side"). It must sit on the same adapter as the game, or the
// game can't open the shared textures: the layer reports the game's adapter,
// and the host re-creates this device on it.
#pragma once

#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <memory>
#include <optional>
#include <string>

namespace OpenDesktopVR::Render {

struct GpuOptions {
  // The adapter to use; the default adapter if unset (or not found)
  std::optional<LUID> mAdapter;
  // Fall back to WARP (software) if no hardware device can be made: tests
  // and CI machines without a GPU
  bool mAllowWarp {false};
};

class GpuDevice {
 public:
  // Null on failure, which is logged
  static std::unique_ptr<GpuDevice> Create(const GpuOptions& options) noexcept;

  ID3D11Device5* Device() const noexcept {
    return mDevice.get();
  }
  ID3D11DeviceContext4* Context() const noexcept {
    return mContext.get();
  }
  ID2D1Factory3* D2DFactory() const noexcept {
    return mD2DFactory.get();
  }
  ID2D1DeviceContext2* D2D() const noexcept {
    return mD2D.get();
  }
  IDWriteFactory* DWrite() const noexcept {
    return mDWrite.get();
  }
  LUID AdapterLuid() const noexcept {
    return mAdapterLuid;
  }
  const std::string& AdapterName() const noexcept {
    return mAdapterName;
  }
  bool IsWarp() const noexcept {
    return mWarp;
  }

 private:
  GpuDevice() = default;

  winrt::com_ptr<ID3D11Device5> mDevice;
  winrt::com_ptr<ID3D11DeviceContext4> mContext;
  winrt::com_ptr<ID2D1Factory3> mD2DFactory;
  winrt::com_ptr<ID2D1Device2> mD2DDevice;
  winrt::com_ptr<ID2D1DeviceContext2> mD2D;
  winrt::com_ptr<IDWriteFactory> mDWrite;
  LUID mAdapterLuid {};
  std::string mAdapterName;
  bool mWarp {false};
};

bool SameLuid(const LUID& a, const LUID& b) noexcept;

}// namespace OpenDesktopVR::Render
