// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// What a document panel can show (M8a, doc 04 "Documents", ADR-047): PDFs,
// images in any format Windows has a decoder for, and folders of them. And
// the limits on what's read, since every file is outside input (TH-9).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Documents {

// A file is read whole into memory, so the user's file is never held open
// (DOC-7); anything larger is refused
inline constexpr std::uintmax_t MaxFileBytes = 256ull << 20;
// A folder's files, in order: the rest are left out
inline constexpr std::size_t MaxFolderFiles = 1000;
// An image this many pixels or more isn't decoded
inline constexpr uint64_t MaxImagePixels = 400'000'000;
// How many pixels wide a page is drawn: from the panel's width in VR, so
// text is sharp in the headset (doc 08: "at the panel's actual pixel
// size"), within these bounds
inline constexpr float PixelsPerMeter = 3500.0f;
inline constexpr uint32_t MinPageWidth = 256;
inline constexpr uint32_t MaxPageWidth = 4096;
// No drawn page is taller than this: a very tall one is drawn narrower
inline constexpr uint32_t MaxPageHeight = 8192;
// A page whose size is unknown or zero is shown in Letter's shape (DOC-3)
inline constexpr float PlaceholderAspect = 8.5f / 11.0f;// width / height

enum class FileKind { None, Pdf, Image };

// The image extensions Windows' installed decoders read (doc 04: taken from
// the codecs, not hard-coded), lower case with the dot (".png"). Asked once.
const std::vector<std::wstring>& ImageExtensions();
// By extension: ".pdf", one of ImageExtensions(), or None
FileKind KindOf(const std::filesystem::path& path);
// Explorer's order (doc 04: "2 before 10"): numbers by value, case ignored
bool NaturalLess(std::wstring_view a, std::wstring_view b) noexcept;
// The pixel width a panel `widthMeters` wide draws its pages at
uint32_t PageWidthFor(float widthMeters) noexcept;
// The size to draw a page of `aspect` (width / height) at `width` pixels:
// never taller than MaxPageHeight, never 0 either way, and Letter's shape if
// `aspect` isn't a usable number (DOC-3)
struct DrawSize {
  uint32_t mWidth {0};
  uint32_t mHeight {0};
};
DrawSize DrawSizeFor(float aspect, uint32_t width) noexcept;

}// namespace OpenDesktopVR::Documents
