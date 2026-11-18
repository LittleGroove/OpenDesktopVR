// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// SavePng is ported from the author's prototype (tests/support/save-png.hpp,
// handover v2), made non-throwing.

#include <OpenDesktopVR/ImageFile.hpp>

#include <winrt/base.h>

#include <Windows.h>

#include <wincodec.h>

#include <cstring>

namespace OpenDesktopVR::ImageFile {

std::optional<Image> ReadTexture(
  ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, bool opaque) noexcept {
  try {
    if (!device || !context || !texture) {
      return std::nullopt;
    }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        && desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS) {
      return std::nullopt;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc = {1, 0};
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) {
      return std::nullopt;
    }
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return std::nullopt;
    }
    Image image {desc.Width, desc.Height};
    const std::size_t row = static_cast<std::size_t>(desc.Width) * 4;
    image.mBgra.resize(row * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
      std::memcpy(image.mBgra.data() + y * row, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, row);
    }
    context->Unmap(staging.get(), 0);
    if (opaque) {
      for (std::size_t i = 3; i < image.mBgra.size(); i += 4) {
        image.mBgra[i] = 0xFF;
      }
    }
    return image;
  } catch (...) {
    return std::nullopt;
  }
}

bool SavePng(const std::filesystem::path& path, const Image& image) noexcept {
  try {
    if (image.mWidth == 0 || image.mHeight == 0
        || image.mBgra.size() < static_cast<std::size_t>(image.mWidth) * image.mHeight * 4) {
      return false;
    }
    // S_FALSE (already initialized) and RPC_E_CHANGED_MODE are both fine:
    // either way COM works on this thread
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(com);
    bool ok = false;
    {
      winrt::com_ptr<IWICImagingFactory> factory;
      winrt::com_ptr<IWICStream> stream;
      winrt::com_ptr<IWICBitmapEncoder> encoder;
      winrt::com_ptr<IWICBitmapFrameEncode> frame;
      WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
      ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))
        && SUCCEEDED(factory->CreateStream(stream.put()))
        && SUCCEEDED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))
        && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()))
        && SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(frame.put(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr))
        && SUCCEEDED(frame->SetSize(image.mWidth, image.mHeight)) && SUCCEEDED(frame->SetPixelFormat(&format))
        && SUCCEEDED(frame->WritePixels(image.mHeight, image.mWidth * 4, static_cast<UINT>(image.mBgra.size()),
          const_cast<BYTE*>(image.mBgra.data())))
        && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (uninitialize) {
      CoUninitialize();
    }
    return ok;
  } catch (...) {
    return false;
  }
}

std::optional<Image> Load(const std::filesystem::path& path) noexcept {
  try {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(com);
    std::optional<Image> loaded;
    {
      winrt::com_ptr<IWICImagingFactory> factory;
      winrt::com_ptr<IWICBitmapDecoder> decoder;
      winrt::com_ptr<IWICBitmapFrameDecode> frame;
      winrt::com_ptr<IWICFormatConverter> converter;
      UINT width = 0;
      UINT height = 0;
      if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))
          && SUCCEEDED(factory->CreateDecoderFromFilename(
            path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))
          && SUCCEEDED(decoder->GetFrame(0, frame.put())) && SUCCEEDED(factory->CreateFormatConverter(converter.put()))
          && SUCCEEDED(converter->Initialize(
            frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
          && SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0) {
        Image image {width, height};
        image.mBgra.resize(static_cast<std::size_t>(width) * height * 4);
        if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.mBgra.size()), image.mBgra.data()))) {
          loaded = std::move(image);
        }
      }
    }
    if (uninitialize) {
      CoUninitialize();
    }
    return loaded;
  } catch (...) {
    return std::nullopt;
  }
}

}// namespace OpenDesktopVR::ImageFile
