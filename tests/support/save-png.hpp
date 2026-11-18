// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
#pragma once

#include <winrt/base.h>

#include <Windows.h>

#include <wincodec.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace OpenDesktopVR::TestUtilities {

// Saves 8-bit BGRA pixels as a PNG; COM must already be initialized
inline void SavePNG(
  const std::filesystem::path& path,
  uint32_t width,
  uint32_t height,
  const std::vector<uint8_t>& bgra) {
  winrt::com_ptr<IWICImagingFactory> factory;
  winrt::check_hresult(CoCreateInstance(
    CLSID_WICImagingFactory,
    nullptr,
    CLSCTX_INPROC_SERVER,
    IID_PPV_ARGS(factory.put())));
  winrt::com_ptr<IWICStream> stream;
  winrt::check_hresult(factory->CreateStream(stream.put()));
  winrt::check_hresult(
    stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE));
  winrt::com_ptr<IWICBitmapEncoder> encoder;
  winrt::check_hresult(
    factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
  winrt::check_hresult(
    encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
  winrt::com_ptr<IWICBitmapFrameEncode> frame;
  winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
  winrt::check_hresult(frame->Initialize(nullptr));
  winrt::check_hresult(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  winrt::check_hresult(frame->SetPixelFormat(&format));
  winrt::check_hresult(frame->WritePixels(
    height,
    width * 4,
    static_cast<UINT>(bgra.size()),
    const_cast<BYTE*>(bgra.data())));
  winrt::check_hresult(frame->Commit());
  winrt::check_hresult(encoder->Commit());
}

}// namespace OpenDesktopVR::TestUtilities
