// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <dxgi1_6.h>

namespace OpenDesktopVR::Render {

bool SameLuid(const LUID& a, const LUID& b) noexcept {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

namespace {

winrt::com_ptr<IDXGIAdapter1> FindAdapter(const LUID& luid) {
  winrt::com_ptr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), factory.put_void()))) {
    return nullptr;
  }
  winrt::com_ptr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), adapter.put_void()))) {
    return nullptr;
  }
  return adapter;
}

HRESULT CreateD3D(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, winrt::com_ptr<ID3D11Device>& device, winrt::com_ptr<ID3D11DeviceContext>& context) {
  const D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  return D3D11CreateDevice(
    adapter,
    driver,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    levels,
    static_cast<UINT>(std::size(levels)),
    D3D11_SDK_VERSION,
    device.put(),
    nullptr,
    context.put());
}

}// namespace

std::unique_ptr<GpuDevice> GpuDevice::Create(const GpuOptions& options) noexcept {
  try {
    std::unique_ptr<GpuDevice> gpu {new GpuDevice()};
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;

    HRESULT hr = E_FAIL;
    if (options.mAdapter) {
      if (const auto adapter = FindAdapter(*options.mAdapter)) {
        hr = CreateD3D(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, device, context);
      } else {
        Log::Warning("GPU: the game's adapter wasn't found; using the default adapter");
      }
    }
    if (FAILED(hr)) {
      device = nullptr;
      context = nullptr;
      hr = CreateD3D(nullptr, D3D_DRIVER_TYPE_HARDWARE, device, context);
    }
    if (FAILED(hr) && options.mAllowWarp) {
      device = nullptr;
      context = nullptr;
      hr = CreateD3D(nullptr, D3D_DRIVER_TYPE_WARP, device, context);
      gpu->mWarp = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
      Log::Error("GPU: couldn't create a Direct3D 11 device (0x{:08X})", static_cast<uint32_t>(hr));
      return nullptr;
    }
    // Fences need Direct3D 11.3/11.4 (Windows 10 1703 or later)
    if (FAILED(device->QueryInterface(gpu->mDevice.put())) || FAILED(context->QueryInterface(gpu->mContext.put()))) {
      Log::Error("GPU: Direct3D 11.4 isn't available (Windows 10 1703 or later is needed)");
      return nullptr;
    }

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device->QueryInterface(dxgiDevice.put())) && SUCCEEDED(dxgiDevice->GetAdapter(adapter.put()))) {
      DXGI_ADAPTER_DESC desc {};
      if (SUCCEEDED(adapter->GetDesc(&desc))) {
        gpu->mAdapterLuid = desc.AdapterLuid;
        gpu->mAdapterName = StringUtil::Narrow(desc.Description);
      }
    }

    // Only the render thread draws: a single-threaded factory is enough
    const D2D1_FACTORY_OPTIONS factoryOptions {D2D1_DEBUG_LEVEL_NONE};
    if (
      FAILED(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &factoryOptions, gpu->mD2DFactory.put_void()))
      || FAILED(gpu->mD2DFactory->CreateDevice(dxgiDevice.get(), gpu->mD2DDevice.put()))
      || FAILED(gpu->mD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, gpu->mD2D.put()))) {
      Log::Error("GPU: couldn't set up Direct2D");
      return nullptr;
    }
    // Text in VR: grayscale antialiasing, never ClearType (RN-13)
    gpu->mD2D->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(gpu->mDWrite.put())))) {
      Log::Error("GPU: couldn't set up DirectWrite");
      return nullptr;
    }
    Log::Info("GPU: {}{}", gpu->mAdapterName, gpu->mWarp ? " (WARP, software)" : "");
    return gpu;
  } catch (...) {
    Log::Error("GPU: device setup threw");
    return nullptr;
  }
}

}// namespace OpenDesktopVR::Render
