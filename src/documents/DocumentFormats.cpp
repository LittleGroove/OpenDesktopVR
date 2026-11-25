// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/DocumentFormats.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <Windows.h>

#include <Shlwapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

namespace OpenDesktopVR::Documents {

namespace {

std::wstring Lower(std::wstring_view text) {
  std::wstring lower {text};
  std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return lower;
}

std::vector<std::wstring> AskDecoders() {
  std::vector<std::wstring> extensions;
  // On a thread of its own, in its own apartment: the caller's may be
  // anything, or none
  std::thread asker([&extensions] {
    ThreadUtil::CatchAll("listing image decoders", [&extensions] {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      for (const auto& decoder: winrt::Windows::Graphics::Imaging::BitmapDecoder::GetDecoderInformationEnumerator()) {
        for (const auto& extension: decoder.FileExtensions()) {
          if (extension.size() > 1 && extension.size() < 16 && extension[0] == L'.') {
            extensions.push_back(Lower(extension));
          }
        }
      }
      winrt::uninit_apartment();
    });
  });
  asker.join();
  std::ranges::sort(extensions);
  const auto [first, last] = std::ranges::unique(extensions);
  extensions.erase(first, last);
  std::erase(extensions, L".pdf");// never an image, whatever a codec claims
  return extensions;
}

}// namespace

const std::vector<std::wstring>& ImageExtensions() {
  static const std::vector<std::wstring> extensions = [] {
    auto found = AskDecoders();
    if (found.empty()) {
      Log::Warning("Documents: Windows lists no image decoders, so no images can be shown");
    }
    return found;
  }();
  return extensions;
}

FileKind KindOf(const std::filesystem::path& path) {
  const auto extension = Lower(path.extension().wstring());
  if (extension == L".pdf") {
    return FileKind::Pdf;
  }
  if (!extension.empty() && std::ranges::binary_search(ImageExtensions(), extension)) {
    return FileKind::Image;
  }
  return FileKind::None;
}

bool NaturalLess(std::wstring_view a, std::wstring_view b) noexcept {
  try {
    return StrCmpLogicalW(std::wstring {a}.c_str(), std::wstring {b}.c_str()) < 0;
  } catch (...) {
    return a < b;
  }
}

uint32_t PageWidthFor(float widthMeters) noexcept {
  if (!std::isfinite(widthMeters) || widthMeters <= 0) {
    return MinPageWidth;
  }
  const double pixels = std::round(static_cast<double>(widthMeters) * PixelsPerMeter);
  return static_cast<uint32_t>(std::clamp(pixels, static_cast<double>(MinPageWidth), static_cast<double>(MaxPageWidth)));
}

DrawSize DrawSizeFor(float aspect, uint32_t width) noexcept {
  if (!std::isfinite(aspect) || aspect <= 0.0f) {
    aspect = PlaceholderAspect;
  }
  width = std::clamp(width, 1u, MaxPageWidth);
  double height = std::round(static_cast<double>(width) / aspect);
  double drawnWidth = width;
  if (height > MaxPageHeight) {
    height = MaxPageHeight;
    drawnWidth = std::round(height * aspect);
  }
  return {static_cast<uint32_t>(std::clamp(drawnWidth, 1.0, static_cast<double>(MaxPageWidth))),
    static_cast<uint32_t>(std::clamp(height, 1.0, static_cast<double>(MaxPageHeight)))};
}

}// namespace OpenDesktopVR::Documents
