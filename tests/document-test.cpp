// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Documents (M8a, ADR-047; doc 04 "Documents"; DOC-3, DOC-7). Every PDF and
// image is made here, so nothing outside the repository is needed.
// - Formats: image extensions from Windows' decoders; Explorer's order
//   ("2 before 10"); drawing widths and sizes, never 0, and Letter's shape
//   for a page with no size (DOC-3).
// - The page cache: within its budget, least recently used first, the page
//   in view never dropped.
// - Pages stepped and kept within the document.
// - PDFs: page count, shapes and colours; damaged, empty, missing and
//   unknown files refused, each with its reason (DOC-3); a page with no
//   size.
// - Images: drawn at the width asked, in their colours.
// - A folder: Explorer's order, a PDF's pages in its place, a damaged PDF's
//   page saying why, hidden and other files left out.
// - The source (the GPU, or WARP): the first page shown and drawn, the next
//   drawn ahead so a flip is instant, a 300-page PDF flipped through briskly
//   never waits (M8's exit), a document that can't open says why on the
//   panel, and removing a panel mid-draw doesn't wait for the page.
//
//   document-test

#include "check.hpp"
#include "save-png.hpp"

#include <OpenDesktopVR/Document.hpp>
#include <OpenDesktopVR/DocumentFormats.hpp>
#include <OpenDesktopVR/DocumentSource.hpp>
#include <OpenDesktopVR/DocumentStatus.hpp>
#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/PageCache.hpp>

#include <winrt/Windows.Data.Pdf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Documents;
using namespace OpenDesktopVR::Testing;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int gSkipped = 0;

// ---- Making files ------------------------------------------------------------

struct PageSpec {
  float mWidth {612};// points, as PDF counts them
  float mHeight {792};
  uint32_t mColor {0xFF0000};// 0xRRGGBB, the whole page
  bool mText {true};
};

// A PDF of these pages: each one filled with its colour, with a line of
// text, written by hand (with its cross-reference table's byte offsets)
std::string MakePdf(std::span<const PageSpec> pages) {
  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<std::size_t> offsets;
  const auto object = [&](const std::string& body) {
    offsets.push_back(out.size());
    out += std::format("{} 0 obj\n{}\nendobj\n", offsets.size(), body);
  };
  const auto pageObject = [](std::size_t i) { return 4 + 2 * i; };
  object("<< /Type /Catalog /Pages 2 0 R >>");
  std::string kids;
  for (std::size_t i = 0; i < pages.size(); ++i) {
    kids += std::format("{} 0 R ", pageObject(i));
  }
  object(std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kids, pages.size()));
  object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  for (std::size_t i = 0; i < pages.size(); ++i) {
    const auto& page = pages[i];
    object(std::format("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {:g} {:g}] /Contents {} 0 R "
                       "/Resources << /Font << /F1 3 0 R >> >> >>",
      page.mWidth, page.mHeight, pageObject(i) + 1));
    auto content = std::format("{:.4f} {:.4f} {:.4f} rg 0 0 {:g} {:g} re f", ((page.mColor >> 16) & 0xFF) / 255.0,
      ((page.mColor >> 8) & 0xFF) / 255.0, (page.mColor & 0xFF) / 255.0, page.mWidth, page.mHeight);
    if (page.mText) {
      content += std::format("\nBT 0 0 0 rg /F1 18 Tf 36 36 Td (Page {} of {}) Tj ET", i + 1, pages.size());
    }
    object(std::format("<< /Length {} >>\nstream\n{}\nendstream", content.size(), content));
  }
  const auto xref = out.size();
  out += std::format("xref\n0 {}\n0000000000 65535 f \n", offsets.size() + 1);
  for (const auto offset: offsets) {
    out += std::format("{:010} 00000 n \n", offset);
  }
  out += std::format("trailer\n<< /Size {} /Root 1 0 R >>\nstartxref\n{}\n%%EOF\n", offsets.size() + 1, xref);
  return out;
}

void WriteBytes(const fs::path& path, std::string_view bytes) {
  std::ofstream file {path, std::ios::binary | std::ios::trunc};
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// An image: the left half one colour, the right half another
void WritePng(const fs::path& path, uint32_t width, uint32_t height, uint32_t left, uint32_t right) {
  std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t rgb = x < width / 2 ? left : right;
      auto* p = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      p[0] = rgb & 0xFF;
      p[1] = (rgb >> 8) & 0xFF;
      p[2] = (rgb >> 16) & 0xFF;
      p[3] = 0xFF;
    }
  }
  TestUtilities::SavePNG(path, width, height, pixels);
}

// ---- Reading pixels ------------------------------------------------------------

uint32_t PixelAt(const PageImage& image, uint32_t x, uint32_t y) {
  const auto* p = image.mPixels.data() + (static_cast<std::size_t>(y) * image.mWidth + x) * 4;
  return (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

bool Near(uint32_t a, uint32_t b, int tolerance = 10) {
  for (int shift: {0, 8, 16}) {
    if (std::abs(static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF)) > tolerance) {
      return false;
    }
  }
  return true;
}

std::string Hex(uint32_t rgb) {
  return std::format("#{:06X}", rgb);
}

// The middle of the page's top half: clear of the text near its foot
uint32_t PageColor(const PageImage& image) {
  return PixelAt(image, image.mWidth / 2, image.mHeight / 4);
}

// ---- The source on the GPU -------------------------------------------------------

// Asks the source (as the render thread does) until `done`, or `limit`
template <class Done>
bool WaitShown(DocumentSource& source, Done&& done, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    source.Prepare();
    if (done(source.ShownNow())) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

// What the source draws: rendered into a texture and read back
std::optional<uint32_t> DrawnColor(Render::GpuDevice& gpu, DocumentSource& source) {
  const auto size = source.Size();
  auto* context = gpu.D2D();
  winrt::com_ptr<ID2D1Bitmap1> target;
  const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  if (FAILED(context->CreateBitmap({size.mWidth, size.mHeight}, nullptr, 0,
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format), target.put()))) {
    return std::nullopt;
  }
  context->SetTarget(target.get());
  context->BeginDraw();
  source.Render(context, size, TimeUtil::Now());
  const HRESULT drawn = context->EndDraw();
  context->SetTarget(nullptr);
  winrt::com_ptr<ID2D1Bitmap1> readable;
  if (FAILED(drawn)
      || FAILED(context->CreateBitmap({size.mWidth, size.mHeight}, nullptr, 0,
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format), readable.put()))
      || FAILED(readable->CopyFromBitmap(nullptr, target.get(), nullptr))) {
    return std::nullopt;
  }
  D2D1_MAPPED_RECT mapped {};
  if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
    return std::nullopt;
  }
  const auto* p = mapped.bits + static_cast<std::size_t>(size.mHeight / 4) * mapped.pitch + (size.mWidth / 2) * 4;
  const uint32_t rgb = (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
  readable->Unmap();
  return rgb;
}

// Windows.Data.Pdf is there (Windows Server without the desktop may lack it)
bool PdfAvailable() {
  try {
    winrt::Windows::Data::Pdf::PdfPageRenderOptions options;
    return true;
  } catch (...) {
    return false;
  }
}

constexpr uint32_t Colors[] {0xD03030, 0x30A040, 0x3050D0, 0xE0C020, 0x9040B0, 0x20B0B0};

}// namespace

int wmain() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  TempFolder temp;
  const fs::path root {temp.Path()};

  Scenario("Formats: images from Windows' decoders, Explorer's order, sizes never 0 (DOC-3)", [&] {
    const auto& extensions = ImageExtensions();
    const auto has = [&](const wchar_t* e) { return std::ranges::binary_search(extensions, std::wstring {e}); };
    Check(has(L".png") && has(L".jpg") && has(L".bmp") && has(L".gif") && has(L".tif"),
      std::format("Windows' decoders give PNG, JPEG, BMP, GIF and TIFF ({} extensions)", extensions.size()));
    Check(!has(L".pdf") && std::ranges::all_of(extensions, [](const auto& e) { return e.starts_with(L"."); }),
      "never .pdf; each with its dot");
    Check(KindOf(L"C:\\a\\Manual.PDF") == FileKind::Pdf && KindOf(L"x.Png") == FileKind::Image
        && KindOf(L"notes.txt") == FileKind::None && KindOf(L"noextension") == FileKind::None,
      "a file's kind by its extension, case ignored");
    Check(NaturalLess(L"page2.png", L"page10.png") && !NaturalLess(L"page10.png", L"page2.png")
        && NaturalLess(L"Alpha", L"beta") && !NaturalLess(L"a", L"A") && !NaturalLess(L"A", L"a"),
      "2 before 10, and case ignored");
    Check(PageWidthFor(0.36f) == 1260 && PageWidthFor(0.01f) == MinPageWidth && PageWidthFor(5.0f) == MaxPageWidth
        && PageWidthFor(0) == MinPageWidth && PageWidthFor(std::nanf("")) == MinPageWidth && PageWidthFor(-1) == MinPageWidth,
      "a page's width from the panel's: 1260 for 36 cm, kept within bounds, any nonsense the smallest");
    const auto letter = DrawSizeFor(PlaceholderAspect, 850);
    const auto none = DrawSizeFor(0, 850);
    const auto nan = DrawSizeFor(std::nanf(""), 850);
    const auto tall = DrawSizeFor(0.01f, 2000);
    const auto wide = DrawSizeFor(100.0f, 2000);
    Check(letter.mWidth == 850 && letter.mHeight == 1100, std::format("Letter at 850: 850 x {}", letter.mHeight));
    Check(none.mWidth == 850 && none.mHeight == 1100 && nan.mHeight == 1100, "no shape, or nonsense: Letter's (DOC-3)");
    Check(tall.mHeight == MaxPageHeight && tall.mWidth >= 1 && tall.mWidth < 2000,
      std::format("a very tall page is drawn narrower: {} x {}", tall.mWidth, tall.mHeight));
    Check(wide.mWidth == 2000 && wide.mHeight == 20, std::format("a very wide one keeps its width: {} x {}", wide.mWidth, wide.mHeight));
    const auto zero = DrawSizeFor(1.0f, 0);
    Check(zero.mWidth >= 1 && zero.mHeight >= 1, "no width asked: still at least 1 x 1");
  });

  Scenario("The page cache: within its budget, least recently used first, the page in view kept", [&] {
    const auto image = [](std::size_t bytes) {
      auto page = std::make_shared<PageImage>();
      page->mWidth = 1;
      page->mHeight = static_cast<uint32_t>(bytes / 4);
      page->mPixels.resize(bytes);
      return std::shared_ptr<const PageImage>(std::move(page));
    };
    PageCache cache {1000};
    cache.Put(0, 100, image(400), 0);
    cache.Put(1, 100, image(400), 0);
    Check(cache.Count() == 2 && cache.Bytes() == 800, "two pages in");
    cache.Get(1, 100);// 1 is now the most recent, 0 the least, and 0 in view
    cache.Put(2, 100, image(400), 0);
    Check(cache.Has(0, 100) && !cache.Has(1, 100) && cache.Has(2, 100) && cache.Bytes() == 800,
      "over budget: the least recently used goes, but never the page in view");
    cache.Put(2, 100, image(200), 0);
    Check(cache.Bytes() == 600 && cache.Count() == 2, "the same page again replaces it, bytes counted again");
    Check(!cache.Has(2, 200), "a page is kept per width");
    cache.Put(5, 100, image(5000), 5);
    Check(cache.Has(5, 100) && !cache.Has(0, 100) && !cache.Has(2, 100),
      "a page in view larger than the budget stays, alone");
    cache.Put(6, 100, image(5000), 5);
    Check(!cache.Has(6, 100) && cache.Has(5, 100), "one ahead that can't fit isn't kept");
    cache.Clear();
    Check(cache.Count() == 0 && cache.Bytes() == 0, "cleared");
  });

  Scenario("Pages stepped, kept within the document", [&] {
    Check(StepPage(0, 1, 5) == 1 && StepPage(4, 1, 5) == 4 && StepPage(0, -1, 5) == 0, "next and previous stop at the ends");
    Check(StepPage(9, -1, 5) == 3 && ClampPage(9, 5) == 4, "a page past the end (the file shrank) steps from the last");
    Check(StepPage(3, 1, 0) == 4 && StepPage(0, -1, 0) == 0 && ClampPage(7, 0) == 7, "not counted yet: never below the first");
    Check(StepPage(1, 10, 5) == 4 && StepPage(4, -10, 5) == 0, "big steps stop at the ends too");
  });

  const bool pdf = PdfAvailable();
  if (!pdf) {
    ++gSkipped;
    std::printf("\n  skip: Windows.Data.Pdf isn't on this machine, so the PDF scenarios don't run\n");
  }
  Canceller canceller;

  if (pdf) {
    Scenario("A PDF: its pages counted, drawn in their shapes and colours", [&] {
      const PageSpec pages[] {{612, 792, Colors[0]}, {792, 612, Colors[1]}, {300, 300, Colors[2]}};
      const auto path = root / L"Three pages.pdf";
      WriteBytes(path, MakePdf(pages));
      auto opened = Document::Open(path, canceller);
      if (!Check(opened.has_value(), opened ? "opened" : "opened: " + opened.error().mText)) {
        return;
      }
      auto& document = **opened;
      Check(document.PageCount() == 3, std::format("3 pages ({})", document.PageCount()));
      const auto portrait = document.Draw(0, 612, canceller);
      const auto landscape = document.Draw(1, 612, canceller);
      const auto square = document.Draw(2, 400, canceller);
      Check(portrait && portrait->mWidth == 612 && portrait->mHeight == 792,
        portrait ? std::format("portrait at 612 wide: {} x {}", portrait->mWidth, portrait->mHeight) : portrait.error());
      Check(landscape && landscape->mWidth == 612 && landscape->mHeight == 473,
        landscape ? std::format("landscape: {} x {}", landscape->mWidth, landscape->mHeight) : landscape.error());
      Check(square && square->mWidth == 400 && square->mHeight == 400,
        square ? std::format("square at 400: {} x {}", square->mWidth, square->mHeight) : square.error());
      if (portrait && landscape && square) {
        Check(Near(PageColor(*portrait), Colors[0]) && Near(PageColor(*landscape), Colors[1]) && Near(PageColor(*square), Colors[2]),
          std::format("each in its colour ({}, {}, {})", Hex(PageColor(*portrait)), Hex(PageColor(*landscape)), Hex(PageColor(*square))));
        Check(portrait->mPixels[3] == 0xFF && portrait->mPixels[portrait->mPixels.size() - 1] == 0xFF, "opaque");
      }
      const auto beyond = document.Draw(3, 612, canceller);
      Check(!beyond && beyond.error() == "There's no such page", "a page past the end: refused, with why");
    });

    Scenario("Damaged, empty, missing and unknown files: refused, each with its reason (DOC-3)", [&] {
      const auto damaged = root / L"damaged.pdf";
      const auto whole = MakePdf(std::array {PageSpec {}});
      WriteBytes(damaged, "%PDF-1.4\nthis isn't a PDF at all\n" + whole.substr(0, whole.size() / 3));
      const auto empty = root / L"empty.pdf";
      WriteBytes(empty, "");
      const auto notes = root / L"notes.txt";
      WriteBytes(notes, "hello");
      const auto a = Document::Open(damaged, canceller);
      const auto b = Document::Open(empty, canceller);
      const auto c = Document::Open(root / L"not there.pdf", canceller);
      const auto d = Document::Open(notes, canceller);
      Check(!a && a.error().mText.find("can't read it as a PDF") != std::string::npos,
        a ? "a damaged PDF opened" : "damaged: " + a.error().mText);
      Check(!b && b.error().mText == "It's empty" && b.error().mRetry, b ? "an empty file opened" : "empty: " + b.error().mText);
      Check(!c && c.error().mText == "Can't find it" && c.error().mRetry, c ? "a missing file opened" : "missing: " + c.error().mText);
      Check(!d && !d.error().mRetry, d ? "a text file opened" : "not a document: " + d.error().mText + " (not tried again)");
    });

    Scenario("A page with no size: why, never a divide by zero (DOC-3)", [&] {
      const PageSpec pages[] {{612, 792, Colors[0]}, {0, 0, Colors[1], false}};
      const auto path = root / L"no size.pdf";
      WriteBytes(path, MakePdf(pages));
      auto opened = Document::Open(path, canceller);
      if (!opened) {
        Check(true, "Windows refuses the whole file: " + opened.error().mText);
        return;
      }
      const auto first = (*opened)->Draw(0, 400, canceller);
      const auto second = (*opened)->Draw(1, 400, canceller);
      Check(first.has_value(), "the page with a size is drawn");
      Check(!second || (second->mWidth >= 1 && second->mHeight >= 1 && second->mHeight <= MaxPageHeight),
        second ? std::format("Windows gives it a size: {} x {}", second->mWidth, second->mHeight) : "refused: " + second.error());
    });
  }

  Scenario("An image: drawn at the width asked, in its colours", [&] {
    const auto path = root / L"Map.png";
    WritePng(path, 64, 32, 0xC02020, 0x2040C0);
    auto opened = Document::Open(path, canceller);
    if (!Check(opened.has_value(), opened ? "opened" : "opened: " + opened.error().mText)) {
      return;
    }
    Check((*opened)->PageCount() == 1, "one page");
    const auto drawn = (*opened)->Draw(0, 256, canceller);
    if (!Check(drawn && drawn->mWidth == 256 && drawn->mHeight == 128,
          drawn ? std::format("256 x 128 ({} x {})", drawn->mWidth, drawn->mHeight) : drawn.error())) {
      return;
    }
    Check(Near(PixelAt(*drawn, 32, 64), 0xC02020) && Near(PixelAt(*drawn, 224, 64), 0x2040C0),
      std::format("left and right in their colours ({}, {})", Hex(PixelAt(*drawn, 32, 64)), Hex(PixelAt(*drawn, 224, 64))));
    const auto broken = root / L"broken.png";
    WriteBytes(broken, "\x89PNG\r\n\x1A\nnot really");
    auto brokenOpened = Document::Open(broken, canceller);
    Check(brokenOpened.has_value(), "a damaged image opens (it's read when drawn)");
    if (brokenOpened) {
      const auto page = (*brokenOpened)->Draw(0, 256, canceller);
      Check(!page && page.error().find("can't read this image") != std::string::npos,
        page ? "a damaged image was drawn" : "drawn: no, " + page.error());
    }
  });

  Scenario("A folder: Explorer's order, a PDF's pages in its place, other files left out", [&] {
    const auto folder = root / L"Binder";
    fs::create_directories(folder);
    WritePng(folder / L"page10.png", 20, 20, Colors[3], Colors[3]);
    WritePng(folder / L"page2.png", 20, 20, Colors[4], Colors[4]);
    WriteBytes(folder / L"notes.txt", "not a page");
    WritePng(folder / L"hidden.png", 20, 20, Colors[5], Colors[5]);
    SetFileAttributesW((folder / L"hidden.png").c_str(), FILE_ATTRIBUTE_HIDDEN);
    fs::create_directories(folder / L"sub");
    WritePng(folder / L"sub" / L"page1.png", 20, 20, Colors[5], Colors[5]);
    if (pdf) {
      const PageSpec two[] {{612, 792, Colors[0]}, {612, 792, Colors[1]}};
      WriteBytes(folder / L"a-checklist.pdf", MakePdf(two));
      WriteBytes(folder / L"b-damaged.pdf", "%PDF-1.4\nnothing here\n");
    }
    auto opened = Document::Open(folder, canceller);
    if (!Check(opened.has_value(), opened ? "opened" : "opened: " + opened.error().mText)) {
      return;
    }
    auto& document = **opened;
    const uint32_t expected = pdf ? 5 : 2;
    Check(document.PageCount() == expected,
      std::format("{} pages: the PDF's 2, the damaged one's 1, the two images; not the text, hidden or subfolder files ({})",
        expected, document.PageCount()));
    std::vector<uint32_t> expectedColors {Colors[4], Colors[3]};
    if (pdf) {
      expectedColors = {Colors[0], Colors[1], 0, Colors[4], Colors[3]};
    }
    std::string seen;
    bool inOrder = document.PageCount() == expected;
    for (uint32_t i = 0; inOrder && i < expected; ++i) {
      const auto page = document.Draw(i, 100, canceller);
      if (expectedColors[i] == 0) {
        inOrder = inOrder && !page && page.error().find("b-damaged.pdf") != std::string::npos;
        seen += page ? " drawn!" : " (" + page.error() + ")";
      } else {
        inOrder = inOrder && page && Near(PageColor(*page), expectedColors[i]);
        seen += page ? " " + Hex(PageColor(*page)) : " " + page.error();
      }
    }
    Check(inOrder, "in order: a-checklist's two pages, the damaged PDF saying why, page2, page10:" + seen);
    const auto empty = root / L"Empty folder";
    fs::create_directories(empty);
    const auto none = Document::Open(empty, canceller);
    Check(!none && none.error().mRetry, none ? "an empty folder opened" : "an empty folder: " + none.error().mText + " (tried again)");
  });

  auto gpu = Render::GpuDevice::Create({.mAllowWarp = true});
  if (!gpu) {
    Check(false, "no GPU device, not even WARP");
    return Finish();
  }

  if (pdf) {
    const auto manual = root / L"Manual.pdf";
    std::vector<PageSpec> pages(300);
    for (std::size_t i = 0; i < pages.size(); ++i) {
      pages[i].mColor = Colors[i % std::size(Colors)];
    }
    WriteBytes(manual, MakePdf(pages));
    const auto board = std::make_shared<StatusBoard>();
    std::atomic<int> dirty {0};

    Scenario("The source: the first page shown, the next drawn ahead, a flip instant", [&] {
      DocumentSource source {*gpu, {.mPath = manual, .mName = L"Manual", .mPanelID = 7, .mView = {0, 630}, .mBoard = board},
        [&dirty] { ++dirty; }};
      Check(WaitShown(source, [](const auto& shown) { return shown.mImage && shown.mPage == 0; }, 20s), "page 1 shown");
      const auto status = board->Get(7);
      Check(status && status->mState == DocumentState::Ready && status->mPageCount == 300,
        std::format("the board: ready, 300 pages ({})", status ? status->mPageCount : 0));
      Check(dirty > 0, "it asked to be drawn");
      const auto size = source.Size();
      Check(size.mWidth == 630 && size.mHeight == 815, std::format("drawn 630 wide: {} x {}", size.mWidth, size.mHeight));
      const auto color = DrawnColor(*gpu, source);
      Check(color && Near(*color, Colors[0]), color ? "on the panel, in its colour: " + Hex(*color) : "drawn on the panel");
      // Pages 2 and 3 are drawn ahead meanwhile
      Check(WaitShown(source, [&](const auto&) { return source.PagesDrawn() >= 3; }, 10s),
        std::format("drawn ahead ({} pages drawn)", source.PagesDrawn()));
      const auto start = std::chrono::steady_clock::now();
      source.SetPageView({1, 630});
      const auto result = source.Prepare();
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      Check(result == Render::Readiness::Ready && source.ShownNow().mPage == 1 && source.ShownNow().mImage,
        std::format("the flip to page 2 is there at once ({:.1f} ms)", ms));
      const auto second = DrawnColor(*gpu, source);
      Check(second && Near(*second, Colors[1]), second ? "page 2 on the panel: " + Hex(*second) : "page 2 drawn");
      Check(!source.Describe().empty() && source.Describe() == L"page 2 of 300", "it describes itself: page 2 of 300");
    });
    Check(!board->Get(7).has_value(), "gone from the board once removed");

    Scenario("A 300-page PDF flipped through briskly never waits (M8's exit: no visible stall)", [&] {
      DocumentSource source {*gpu, {.mPath = manual, .mName = L"Manual", .mPanelID = 8, .mView = {0, 1260}}, [] {}};
      if (!Check(WaitShown(source, [](const auto& shown) { return shown.mImage && shown.mPage == 0; }, 20s), "page 1 shown")) {
        return;
      }
      std::vector<double> waits;
      for (uint32_t page = 1; page <= 40; ++page) {
        std::this_thread::sleep_for(250ms);// a page every quarter second: faster than anyone reads
        const auto start = std::chrono::steady_clock::now();
        source.SetPageView({page, 1260});
        WaitShown(source, [&](const auto& shown) { return shown.mImage && shown.mPage == page; }, 5s);
        waits.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      }
      std::ranges::sort(waits);
      const auto slowest = waits.back();
      const auto waited = std::ranges::count_if(waits, [](double ms) { return ms > 11.0; });
      std::printf("    flips: median %.1f ms, slowest %.1f ms, %d over a 90 Hz frame (11 ms)\n", waits[waits.size() / 2], slowest,
        static_cast<int>(waited));
      std::printf("    %s\n", source.StatsText().c_str());
      Check(slowest <= 50.0, std::format("every flip shown within 50 ms: a page drawn ahead each time (slowest {:.1f} ms)", slowest));
    });

    Scenario("A document that can't open says why on the panel; a wider panel draws the page again", [&] {
      const auto damaged = root / L"Broken manual.pdf";
      WriteBytes(damaged, "%PDF-1.4\nnothing\n");
      DocumentSource broken {*gpu, {.mPath = damaged, .mName = L"Broken manual", .mPanelID = 9, .mView = {0, 630}}, [] {}};
      Check(WaitShown(broken, [](const auto& shown) { return shown.mMessage.starts_with(L"Can't show Broken manual"); }, 10s),
        "the panel says it can't show it, and why");
      const auto size = broken.Size();
      Check(size.mWidth > 0 && size.mHeight > size.mWidth, std::format("in Letter's shape ({} x {})", size.mWidth, size.mHeight));
      const auto color = DrawnColor(*gpu, broken);
      Check(color.has_value(), "the message is drawn");

      DocumentSource source {*gpu, {.mPath = manual, .mName = L"Manual", .mPanelID = 10, .mView = {4, 630}}, [] {}};
      Check(WaitShown(source, [](const auto& shown) { return shown.mImage && shown.mPage == 4; }, 20s), "page 5 shown at 630");
      source.SetPageView({4, 900});
      Check(WaitShown(source, [](const auto& shown) { return shown.mImage && shown.mImage->mWidth == 900; }, 20s),
        "wider: drawn again at 900");
      Check(source.Size().mWidth == 900, "and its size follows");
    });

    Scenario("A document not there yet opens once it is; trying again changes nothing until then", [&] {
      const auto later = root / L"Later.pdf";
      auto watched = std::make_shared<StatusBoard>();
      std::atomic<int> changes {0};
      watched->SetOnChange([&changes] { ++changes; });
      DocumentSource source {*gpu, {.mPath = later, .mName = L"Later", .mPanelID = 12, .mView = {0, 630}, .mBoard = watched},
        [] {}};
      Check(WaitShown(source, [](const auto& shown) { return shown.mMessage.starts_with(L"Can't show Later\nCan't find it"); }, 10s),
        "it says it can't find it");
      const int before = changes;
      const auto now = TimeUtil::Now();
      for (int second = 0; second < RetrySeconds; ++second) {
        source.Maintain(now);// as the render loop does, once a second
      }
      std::this_thread::sleep_for(300ms);
      Check(source.Prepare() == Render::Readiness::Unchanged && changes == before,
        std::format("tried again: nothing redrawn, the board unchanged ({} changes)", changes - before));
      const PageSpec one[] {{612, 792, Colors[2]}};
      WriteBytes(later, MakePdf(one));
      for (int second = 0; second < RetrySeconds; ++second) {
        source.Maintain(now);
      }
      Check(WaitShown(source, [](const auto& shown) { return shown.mImage && shown.mPage == 0; }, 10s),
        "once it's there, it opens and shows");
      watched->SetOnChange({});
    });

    Scenario("Removing a panel while its page is drawn doesn't wait for the page", [&] {
      const auto start = std::chrono::steady_clock::now();
      {
        DocumentSource source {*gpu, {.mPath = manual, .mName = L"Manual", .mPanelID = 11, .mView = {150, MaxPageWidth}}, [] {}};
        std::this_thread::sleep_for(30ms);// opening, or drawing the first page
      }
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      Check(ms < 2000, std::format("gone in {:.0f} ms", ms));
    });
  }

  if (gSkipped > 0) {
    std::printf("\n%d skipped\n", gSkipped);
  }
  return Finish();
}
