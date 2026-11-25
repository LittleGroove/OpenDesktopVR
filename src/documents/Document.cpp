// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Document.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <MemoryBuffer.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <span>
#include <vector>

namespace OpenDesktopVR::Documents {

namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage::Streams;

// ---- Canceller -------------------------------------------------------------

void Canceller::Cancel() noexcept {
  std::scoped_lock lock(mMutex);
  mCancelled = true;
  if (mRunning) {
    try {
      mRunning.Cancel();
    } catch (...) {
    }
  }
}

bool Canceller::Cancelled() const noexcept {
  std::scoped_lock lock(mMutex);
  return mCancelled;
}

void Canceller::Begin(const winrt::Windows::Foundation::IAsyncInfo& info) {
  std::scoped_lock lock(mMutex);
  if (mCancelled) {
    try {
      info.Cancel();
    } catch (...) {
    }
    throw winrt::hresult_canceled();
  }
  mRunning = info;
}

void Canceller::End() noexcept {
  std::scoped_lock lock(mMutex);
  mRunning = nullptr;
}

namespace {

// ---- Reading files -----------------------------------------------------------

std::expected<std::vector<uint8_t>, Problem> ReadWhole(const fs::path& path) {
  // Shared every way: a writer, or a sync tool replacing the file, isn't
  // stopped by us
  winrt::file_handle file {CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (!file) {
    const auto error = GetLastError();
    switch (error) {
      case ERROR_FILE_NOT_FOUND:
      case ERROR_PATH_NOT_FOUND:
      case ERROR_NOT_READY:
      case ERROR_BAD_NETPATH:
        return std::unexpected(Problem {"Can't find it", true});
      case ERROR_SHARING_VIOLATION:
      case ERROR_LOCK_VIOLATION:
        return std::unexpected(Problem {"Another program has it open", true});
      case ERROR_ACCESS_DENIED:
        return std::unexpected(Problem {"Windows won't let it be read (access denied)", false});
      default:
        return std::unexpected(Problem {std::format("Couldn't read it (error {})", error), true});
    }
  }
  LARGE_INTEGER size {};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) {
    return std::unexpected(Problem {std::format("Couldn't read it (error {})", GetLastError()), true});
  }
  if (static_cast<std::uintmax_t>(size.QuadPart) > MaxFileBytes) {
    return std::unexpected(Problem {std::format("It's larger than {} MB", MaxFileBytes >> 20), false});
  }
  std::vector<uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t done = 0;
  while (done < bytes.size()) {
    DWORD read = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 16u << 20));
    if (!ReadFile(file.get(), bytes.data() + done, chunk, &read, nullptr)) {
      return std::unexpected(Problem {std::format("Couldn't read it (error {})", GetLastError()), true});
    }
    if (read == 0) {
      break;// it shrank while being read: what was there is used (doc 04)
    }
    done += read;
  }
  bytes.resize(done);
  if (bytes.empty()) {
    return std::unexpected(Problem {"It's empty", true});
  }
  return bytes;
}

InMemoryRandomAccessStream ToStream(std::span<const uint8_t> bytes, Canceller& canceller) {
  InMemoryRandomAccessStream stream;
  DataWriter writer {stream};
  writer.WriteBytes(winrt::array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
  canceller.Wait(writer.StoreAsync());
  writer.DetachStream();
  stream.Seek(0);
  return stream;
}

// ---- Pixels ------------------------------------------------------------------

// The bitmap's pixels, row by row as its buffer lays them out (its stride
// may be wider than the row)
std::expected<PageImage, std::string> ToImage(const SoftwareBitmap& bitmap, bool opaque) {
  PageImage image;
  image.mWidth = static_cast<uint32_t>(bitmap.PixelWidth());
  image.mHeight = static_cast<uint32_t>(bitmap.PixelHeight());
  if (image.mWidth == 0 || image.mHeight == 0 || image.mWidth > MaxPageWidth || image.mHeight > MaxPageHeight) {
    return std::unexpected(std::format("It came out {} x {} pixels", image.mWidth, image.mHeight));
  }
  const auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
  const auto plane = buffer.GetPlaneDescription(0);
  const auto reference = buffer.CreateReference();
  uint8_t* data = nullptr;
  UINT32 capacity = 0;
  winrt::check_hresult(reference.as<::Windows::Foundation::IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity));
  const std::size_t row = static_cast<std::size_t>(image.mWidth) * 4;
  const auto stride = static_cast<std::size_t>(std::max(plane.Stride, 0));
  const auto start = static_cast<std::size_t>(std::max(plane.StartIndex, 0));
  if (!data || stride < row || start + stride * (image.mHeight - 1) + row > capacity) {
    return std::unexpected(std::string {"Its pixels didn't add up"});
  }
  image.mPixels.resize(row * image.mHeight);
  for (uint32_t y = 0; y < image.mHeight; ++y) {
    std::memcpy(image.mPixels.data() + row * y, data + start + stride * y, row);
  }
  if (opaque) {
    // A PDF page is drawn on white; the BMP in between may carry no alpha
    for (std::size_t i = 3; i < image.mPixels.size(); i += 4) {
      image.mPixels[i] = 0xFF;
    }
  }
  return image;
}

std::string Hresult(const winrt::hresult_error& error) {
  return std::format("{:08X}", static_cast<uint32_t>(error.code().value));
}

// ---- PDF ---------------------------------------------------------------------

struct OpenPdf {
  PdfDocument mPdf {nullptr};
  InMemoryRandomAccessStream mStream {nullptr};
};

std::expected<OpenPdf, Problem> LoadPdf(const fs::path& path, Canceller& canceller) {
  auto bytes = ReadWhole(path);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  try {
    OpenPdf open;
    open.mStream = ToStream(*bytes, canceller);
    bytes->clear();
    bytes->shrink_to_fit();
    open.mPdf = canceller.Wait(PdfDocument::LoadFromStreamAsync(open.mStream));
    if (open.mPdf.PageCount() == 0) {
      return std::unexpected(Problem {"It has no pages", true});
    }
    return open;
  } catch (const winrt::hresult_canceled&) {
    throw;
  } catch (const winrt::hresult_error& error) {
    if (canceller.Cancelled()) {
      throw winrt::hresult_canceled();
    }
    if (error.code() == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD)) {
      return std::unexpected(Problem {"It needs a password, which isn't supported yet", false});
    }
    Log::Info("Documents: Windows couldn't read {} as a PDF ({})", StringUtil::Narrow(path.filename().wstring()), Hresult(error));
    return std::unexpected(Problem {"Windows can't read it as a PDF (damaged?)", true});
  }
}

std::expected<PageImage, std::string> DrawPdfPage(const PdfDocument& pdf, uint32_t index, uint32_t width, Canceller& canceller) {
  const auto page = pdf.GetPage(index);
  const auto size = page.Size();// its crop box, rotated, in 1/96 inch
  if (!(size.Width > 0 && size.Height > 0) || !std::isfinite(size.Width) || !std::isfinite(size.Height)) {
    return std::unexpected(std::string {"This page has no size"});
  }
  const auto draw = DrawSizeFor(size.Width / size.Height, width);
  PdfPageRenderOptions options;
  options.DestinationWidth(draw.mWidth);
  options.DestinationHeight(draw.mHeight);
  // Uncompressed in between: the fastest way to the pixels
  options.BitmapEncoderId(BitmapEncoder::BmpEncoderId());
  InMemoryRandomAccessStream drawn;
  canceller.Wait(page.RenderToStreamAsync(drawn, options));
  drawn.Seek(0);
  const auto decoder = canceller.Wait(BitmapDecoder::CreateAsync(BitmapDecoder::BmpDecoderId(), drawn));
  const auto bitmap = canceller.Wait(decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore));
  return ToImage(bitmap, true);
}

// ---- Images ------------------------------------------------------------------

std::expected<PageImage, std::string> DrawImage(const fs::path& path, uint32_t width, Canceller& canceller) {
  auto bytes = ReadWhole(path);
  if (!bytes) {
    return std::unexpected(bytes.error().mText);
  }
  const auto stream = ToStream(*bytes, canceller);
  bytes->clear();
  BitmapDecoder decoder {nullptr};
  try {
    decoder = canceller.Wait(BitmapDecoder::CreateAsync(stream));
  } catch (const winrt::hresult_canceled&) {
    throw;
  } catch (const winrt::hresult_error& error) {
    if (canceller.Cancelled()) {
      throw winrt::hresult_canceled();
    }
    Log::Info("Documents: Windows couldn't read {} as an image ({})", StringUtil::Narrow(path.filename().wstring()), Hresult(error));
    return std::unexpected(std::string {"Windows can't read this image (damaged?)"});
  }
  const uint32_t pixelWidth = decoder.PixelWidth();
  const uint32_t pixelHeight = decoder.PixelHeight();
  const uint32_t orientedWidth = decoder.OrientedPixelWidth();
  const uint32_t orientedHeight = decoder.OrientedPixelHeight();
  if (pixelWidth == 0 || pixelHeight == 0 || orientedWidth == 0 || orientedHeight == 0) {
    return std::unexpected(std::string {"This image has no size"});
  }
  if (static_cast<uint64_t>(pixelWidth) * pixelHeight >= MaxImagePixels) {
    return std::unexpected(std::format("This image is too large ({} x {} pixels)", pixelWidth, pixelHeight));
  }
  const auto draw = DrawSizeFor(static_cast<float>(orientedWidth) / static_cast<float>(orientedHeight), width);
  // One scale both ways, so it doesn't matter whether the turn the photo
  // asks for comes before or after the scaling
  const double scale = static_cast<double>(draw.mWidth) / orientedWidth;
  BitmapTransform transform;
  transform.ScaledWidth(static_cast<uint32_t>(std::max(1.0, std::round(pixelWidth * scale))));
  transform.ScaledHeight(static_cast<uint32_t>(std::max(1.0, std::round(pixelHeight * scale))));
  transform.InterpolationMode(scale < 1.0 ? BitmapInterpolationMode::Fant : BitmapInterpolationMode::Cubic);
  const auto bitmap = canceller.Wait(decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied,
    transform, ExifOrientationMode::RespectExifOrientation, ColorManagementMode::ColorManageToSRgb));
  return ToImage(bitmap, false);
}

}// namespace

// ---- Document ----------------------------------------------------------------

struct Document::Impl {
  struct Entry {
    fs::path mPath;
    FileKind mKind {FileKind::None};
    uint32_t mFirstPage {0};
    uint32_t mPageCount {1};
    std::string mProblem;// it couldn't be opened: its one page says why
  };
  std::vector<Entry> mEntries;
  uint32_t mPageCount {0};
  // One PDF open at a time (a folder's others are opened when reached)
  std::size_t mOpenEntry {SIZE_MAX};
  OpenPdf mOpen;
};

Document::Document() : mImpl(std::make_unique<Impl>()) {
}

// Here, on the document's thread (DOC-7)
Document::~Document() = default;

uint32_t Document::PageCount() const noexcept {
  return mImpl->mPageCount;
}

std::expected<std::unique_ptr<Document>, Problem> Document::Open(const fs::path& path, Canceller& canceller) {
  std::unique_ptr<Document> document {new Document()};
  auto& impl = *document->mImpl;
  std::error_code error;
  const bool folder = fs::is_directory(path, error);
  if (!folder && !fs::exists(path, error)) {
    return std::unexpected(Problem {"Can't find it", true});
  }
  if (folder) {
    // Its documents and images, not its subfolders, in Explorer's order
    std::vector<Impl::Entry> found;
    for (fs::directory_iterator it {path, fs::directory_options::skip_permission_denied, error}, end; !error && it != end;
         it.increment(error)) {
      std::error_code fileError;
      if (!it->is_regular_file(fileError)) {
        continue;
      }
      const auto attributes = GetFileAttributesW(it->path().c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
        continue;
      }
      if (const auto kind = KindOf(it->path()); kind != FileKind::None) {
        found.push_back({it->path(), kind});
      }
    }
    if (error && found.empty()) {
      return std::unexpected(Problem {"Can't read the folder", true});
    }
    std::ranges::sort(found, [](const auto& a, const auto& b) { return NaturalLess(a.mPath.filename().wstring(), b.mPath.filename().wstring()); });
    if (found.size() > MaxFolderFiles) {
      Log::Info("Documents: a folder has {} files to show; the first {} are used", found.size(), MaxFolderFiles);
      found.resize(MaxFolderFiles);
    }
    impl.mEntries = std::move(found);
  } else {
    const auto kind = KindOf(path);
    if (kind == FileKind::None) {
      return std::unexpected(Problem {"It isn't a PDF or an image Windows can read", false});
    }
    impl.mEntries.push_back({path, kind});
  }
  if (impl.mEntries.empty()) {
    return std::unexpected(Problem {"There are no PDFs or images in the folder", true});
  }
  // Page counts: each PDF opened once now (a folder's are let go again)
  uint64_t pages = 0;
  for (std::size_t i = 0; i < impl.mEntries.size(); ++i) {
    auto& entry = impl.mEntries[i];
    entry.mFirstPage = static_cast<uint32_t>(pages);
    entry.mPageCount = 1;
    if (entry.mKind == FileKind::Pdf) {
      auto open = LoadPdf(entry.mPath, canceller);
      if (!open) {
        if (!folder) {
          return std::unexpected(open.error());
        }
        entry.mProblem = std::format("{}: {}", StringUtil::Narrow(entry.mPath.filename().wstring()), open.error().mText);
      } else {
        entry.mPageCount = open->mPdf.PageCount();
        if (!folder) {
          impl.mOpen = std::move(*open);
          impl.mOpenEntry = i;
        }
      }
    }
    pages += entry.mPageCount;
    if (pages > UINT32_MAX / 2) {
      return std::unexpected(Problem {"It has too many pages", false});
    }
  }
  impl.mPageCount = static_cast<uint32_t>(pages);
  return document;
}

std::expected<PageImage, std::string> Document::Draw(uint32_t page, uint32_t width, Canceller& canceller) {
  auto& impl = *mImpl;
  if (page >= impl.mPageCount) {
    return std::unexpected(std::string {"There's no such page"});
  }
  const auto after = std::ranges::upper_bound(impl.mEntries, page, {}, &Impl::Entry::mFirstPage);
  const auto index = static_cast<std::size_t>(after - impl.mEntries.begin()) - 1;
  const auto& entry = impl.mEntries[index];
  if (!entry.mProblem.empty()) {
    return std::unexpected(entry.mProblem);
  }
  try {
    if (entry.mKind == FileKind::Image) {
      return DrawImage(entry.mPath, width, canceller);
    }
    if (impl.mOpenEntry != index) {
      impl.mOpen = {};// the last one goes, here (DOC-7)
      impl.mOpenEntry = SIZE_MAX;
      auto open = LoadPdf(entry.mPath, canceller);
      if (!open) {
        return std::unexpected(open.error().mText);
      }
      impl.mOpen = std::move(*open);
      impl.mOpenEntry = index;
    }
    const uint32_t inFile = page - entry.mFirstPage;
    if (inFile >= impl.mOpen.mPdf.PageCount()) {
      return std::unexpected(std::string {"There's no such page"});
    }
    return DrawPdfPage(impl.mOpen.mPdf, inFile, width, canceller);
  } catch (const winrt::hresult_canceled&) {
    throw;
  } catch (const winrt::hresult_error& error) {
    if (canceller.Cancelled()) {
      throw winrt::hresult_canceled();
    }
    Log::Info("Documents: page {} of {} couldn't be drawn ({})", page + 1, StringUtil::Narrow(entry.mPath.filename().wstring()),
      Hresult(error));
    return std::unexpected(std::string {"This page can't be drawn"});
  }
}

}// namespace OpenDesktopVR::Documents
