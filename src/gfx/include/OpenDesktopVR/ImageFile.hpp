// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Saving what's on screen as an image, and reading it back: the mirror's
// and the settings window's screenshot options, and tests that check them.
// These wait for the GPU or the disk, so never call them from a frame loop
// that mustn't block (the layer, the render thread).
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace OpenDesktopVR::ImageFile {

struct Image {
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  std::vector<uint8_t> mBgra;// 8 bits a channel, rows packed
};

// Copies a B8G8R8A8 texture (or a swapchain back buffer) to memory.
// `opaque` sets alpha to 255, for windows, which have no alpha.
std::optional<Image> ReadTexture(
  ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, bool opaque = true) noexcept;

// Saves as PNG through WIC; initializes COM on this thread if it isn't.
// False on any failure.
bool SavePng(const std::filesystem::path& path, const Image& image) noexcept;

// Loads any image WIC can read, as 8-bit BGRA (tests check screenshots)
std::optional<Image> Load(const std::filesystem::path& path) noexcept;

}// namespace OpenDesktopVR::ImageFile
