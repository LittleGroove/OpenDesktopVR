// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/PanelRing.hpp>

#include <dxgi1_2.h>

namespace OpenDesktopVR::Render {

std::unique_ptr<PanelRing> PanelRing::Create(GpuDevice& gpu, uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept {
  try {
    std::unique_ptr<PanelRing> ring {new PanelRing()};
    ring->mWidth = width;
    ring->mHeight = height;
    ring->mFormat = format;
    const D3D11_TEXTURE2D_DESC desc {
      .Width = width,
      .Height = height,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = format,
      .SampleDesc = {1, 0},
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
      .MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED,
    };
    for (uint32_t slot = 0; slot < Slots; ++slot) {
      auto& texture = ring->mTextures[slot];
      if (FAILED(gpu.Device()->CreateTexture2D(&desc, nullptr, texture.put()))) {
        Log::Error("Panel ring: couldn't create a {}x{} shared texture", width, height);
        return nullptr;
      }
      winrt::com_ptr<IDXGIResource1> resource;
      texture->QueryInterface(resource.put());
      // The game's VRAM pressure mustn't push panels out to system memory
      resource->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);
      if (FAILED(resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &ring->mTextureHandles[slot]))) {
        Log::Error("Panel ring: couldn't share a texture");
        return nullptr;
      }
      winrt::com_ptr<IDXGISurface> surface;
      texture->QueryInterface(surface.put());
      const D2D1_BITMAP_PROPERTIES1 properties {
        .pixelFormat = {format, D2D1_ALPHA_MODE_PREMULTIPLIED},
        .dpiX = 96,
        .dpiY = 96,
        .bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      };
      if (FAILED(gpu.D2D()->CreateBitmapFromDxgiSurface(surface.get(), &properties, ring->mTargets[slot].put()))) {
        Log::Error("Panel ring: Direct2D can't draw into format {}", static_cast<int>(format));
        return nullptr;
      }
    }
    if (FAILED(gpu.Device()->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), ring->mFence.put_void()))
        || FAILED(ring->mFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &ring->mFenceHandle))) {
      Log::Error("Panel ring: couldn't create a shared fence");
      return nullptr;
    }
    return ring;
  } catch (...) {
    Log::Error("Panel ring: setup threw");
    return nullptr;
  }
}

PanelRing::~PanelRing() {
  for (auto handle: mTextureHandles) {
    if (handle) {
      CloseHandle(handle);
    }
  }
  if (mFenceHandle) {
    CloseHandle(mFenceHandle);
  }
}

uint64_t PanelRing::Advance(uint32_t slot) noexcept {
  mNewestSlot = slot;
  return ++mFenceValue;
}

void PanelRing::Signal(ID3D11DeviceContext4* context, uint64_t value) noexcept {
  context->Signal(mFence.get(), value);
  context->Flush();// SY-3: never leave the signal in the command buffer
}

}// namespace OpenDesktopVR::Render
