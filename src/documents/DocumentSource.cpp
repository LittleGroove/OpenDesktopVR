// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Document.hpp>
#include <OpenDesktopVR/DocumentSource.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/Theme.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <format>
#include <map>
#include <mutex>
#include <optional>

namespace OpenDesktopVR::Documents {

namespace {

D2D1_COLOR_F Rgb(uint32_t rgb) {
  const auto c = Theme::ToRgba(rgb);
  return {c.r, c.g, c.b, c.a};
}

// The pages drawn ahead, in this order, after the one in view
constexpr int Ahead[] {1, 2, -1, 3};

}// namespace

struct DocumentSource::Shared {
  explicit Shared(std::size_t cacheBytes) : mCache(cacheBytes) {
  }

  std::mutex mMutex;
  std::condition_variable_any mWake;
  Canceller mCanceller;
  // Asked for (render thread)
  Render::PageView mView;
  bool mActive {true};
  bool mReload {false};
  // Found (the document's thread)
  DocumentStatus mStatus;
  bool mRetry {false};
  PageCache mCache;
  std::map<uint32_t, std::string> mPageProblems;// pages that can't be drawn, and why (DOC-3)
  uint64_t mVersion {0};// a change the render thread should see
  // For the stats line
  uint64_t mDrawn {0};
  uint64_t mDrawnAhead {0};
  double mDrawMsTotal {0};
  double mDrawMsMax {0};
};

DocumentSource::DocumentSource(Render::GpuDevice& gpu, DocumentSourceOptions options, Render::MarkDirty markDirty)
  : mOptions(std::move(options)), mMarkDirty(std::move(markDirty)),
    mShared(std::make_shared<Shared>(mOptions.mCacheBytes)) {
  mDWrite.copy_from(gpu.DWrite());
  if (mOptions.mView.mWidth == 0) {
    mOptions.mView.mWidth = PageWidthFor(0.36f);
  }
  mShared->mView = mOptions.mView;
  if (mOptions.mBoard) {
    mOptions.mBoard->Set(mOptions.mPanelID, {});
  }
  mWorker = std::jthread([this](std::stop_token stop) { Work(std::move(stop)); });
}

DocumentSource::~DocumentSource() {
  // Whatever the thread waits on stops now; the jthread then joins it
  mShared->mCanceller.Cancel();
  mWorker.request_stop();
  if (mOptions.mBoard) {
    mOptions.mBoard->Remove(mOptions.mPanelID);
  }
}

// ---- The document's thread ---------------------------------------------------

void DocumentSource::Work(std::stop_token stop) {
  ThreadUtil::SetCurrentThreadName(L"Document");
  ThreadUtil::CatchAll("document thread", [&] {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto& shared = *mShared;
    const auto name = StringUtil::Narrow(mOptions.mName);
    // Released here, on this thread, whatever ends the loop (DOC-7)
    std::unique_ptr<Document> document;
    // A page too large to keep ahead: drawing ahead stops until the view
    // changes, rather than drawing it again and again
    std::optional<Render::PageView> noRoomFor;
    const auto publish = [&](const DocumentStatus& status) {
      if (mOptions.mBoard) {
        mOptions.mBoard->Set(mOptions.mPanelID, status);
      }
      mMarkDirty();
    };
    while (!stop.stop_requested()) {
      // Open it, or open it again
      bool open = false;
      {
        std::unique_lock lock(shared.mMutex);
        open = shared.mReload || (!document && shared.mStatus.mState == DocumentState::Loading);
        shared.mReload = false;
      }
      if (open) {
        document.reset();// outside the lock: letting a PDF go may take a while
        // A retry after a failure changes nothing that shows unless its
        // result differs: no "Opening..." on the panel every few seconds,
        // no repaint of the settings window, no line in the log
        DocumentStatus before;
        {
          std::unique_lock lock(shared.mMutex);
          before = shared.mStatus;
          if (before.mState != DocumentState::Failed) {
            shared.mStatus = {};
            ++shared.mVersion;
          }
          shared.mCache.Clear();
          shared.mPageProblems.clear();
        }
        if (before.mState != DocumentState::Failed) {
          publish({});
        }
        const auto start = std::chrono::steady_clock::now();
        std::expected<std::unique_ptr<Document>, Problem> opened = std::unexpected(Problem {"Couldn't open it", true});
        try {
          opened = Document::Open(mOptions.mPath, shared.mCanceller);
        } catch (const winrt::hresult_canceled&) {
          break;
        } catch (...) {
          if (stop.stop_requested() || shared.mCanceller.Cancelled()) {
            break;
          }
          Log::Warning("Documents: opening '{}' failed: {}", name, ThreadUtil::DescribeCurrentException());
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        DocumentStatus status;
        {
          std::unique_lock lock(shared.mMutex);
          if (opened) {
            document = std::move(*opened);
            shared.mStatus = {DocumentState::Ready, document->PageCount(), {}};
            shared.mRetry = false;
          } else {
            shared.mStatus = {DocumentState::Failed, 0, opened.error().mText};
            shared.mRetry = opened.error().mRetry;
          }
          status = shared.mStatus;
          if (status != before) {
            ++shared.mVersion;
          }
        }
        if (status == before) {
          continue;// tried again, the same failure
        }
        if (opened) {
          Log::Info("Documents: '{}' opened: {} page{} ({:.0f} ms)", name, status.mPageCount, status.mPageCount == 1 ? "" : "s", ms);
        } else {
          Log::Info("Documents: '{}' can't be shown: {}{}", name, status.mProblem,
            shared.mRetry ? std::format("; tried again every {} s", RetrySeconds) : "");
        }
        publish(status);
        continue;
      }

      // What to draw: the page in view, then the ones around it
      std::optional<uint32_t> job;
      bool inView = false;
      Render::PageView view;
      {
        std::unique_lock lock(shared.mMutex);
        view = shared.mView;
        const auto count = shared.mStatus.mPageCount;
        if (document && count > 0) {
          const auto page = ClampPage(view.mPage, count);
          const auto wanted = [&](uint32_t p) { return !shared.mCache.Has(p, view.mWidth) && !shared.mPageProblems.contains(p); };
          if (wanted(page)) {
            job = page;
            inView = true;
          } else if (shared.mActive && noRoomFor != view) {
            for (const int step: Ahead) {
              const int64_t p = static_cast<int64_t>(page) + step;
              if (p >= 0 && p < count && wanted(static_cast<uint32_t>(p))) {
                job = static_cast<uint32_t>(p);
                break;
              }
            }
          }
        }
        if (!job) {
          // Until the view changes, the panel comes into use, or it's opened again
          const bool active = shared.mActive;
          shared.mWake.wait(lock, stop, [&] { return shared.mReload || shared.mView != view || shared.mActive != active; });
          continue;
        }
      }

      const auto start = std::chrono::steady_clock::now();
      std::expected<PageImage, std::string> drawn = std::unexpected(std::string {"This page can't be drawn"});
      try {
        drawn = document->Draw(*job, view.mWidth, shared.mCanceller);
      } catch (const winrt::hresult_canceled&) {
        break;
      } catch (...) {
        if (stop.stop_requested() || shared.mCanceller.Cancelled()) {
          break;
        }
        Log::Warning("Documents: drawing page {} of '{}' failed: {}", *job + 1, name, ThreadUtil::DescribeCurrentException());
      }
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      bool show = false;
      {
        std::unique_lock lock(shared.mMutex);
        ++shared.mDrawn;
        shared.mDrawnAhead += inView ? 0 : 1;
        shared.mDrawMsTotal += ms;
        shared.mDrawMsMax = std::max(shared.mDrawMsMax, ms);
        const auto inViewNow = ClampPage(shared.mView.mPage, shared.mStatus.mPageCount);
        if (drawn) {
          shared.mCache.Put(*job, view.mWidth, std::make_shared<const PageImage>(std::move(*drawn)), inViewNow);
          if (!shared.mCache.Has(*job, view.mWidth)) {
            noRoomFor = view;
          }
        } else {
          shared.mPageProblems[*job] = drawn.error();
        }
        // Wanted now (perhaps flipped to while it was drawn ahead)
        show = *job == inViewNow && view.mWidth == shared.mView.mWidth;
        if (show) {
          ++shared.mVersion;
        }
      }
      if (show) {
        mMarkDirty();
      }
    }
    document.reset();
  });
}

// ---- The render thread -------------------------------------------------------

void DocumentSource::SetPageView(const Render::PageView& view) {
  {
    std::scoped_lock lock(mShared->mMutex);
    if (mShared->mView == view) {
      return;
    }
    mShared->mView = view;
    ++mShared->mVersion;// Prepare looks again: the page may be ready already
  }
  mShared->mWake.notify_all();
  mMarkDirty();
}

void DocumentSource::SetActive(bool active) {
  {
    std::scoped_lock lock(mShared->mMutex);
    mShared->mActive = active;
  }
  mShared->mWake.notify_all();
}

void DocumentSource::Maintain(TimeUtil::TimePoint) {
  bool retry = false;
  {
    std::scoped_lock lock(mShared->mMutex);
    if (mShared->mStatus.mState == DocumentState::Failed && mShared->mRetry) {
      if (++mFailedSeconds >= RetrySeconds) {
        mFailedSeconds = 0;
        mShared->mReload = true;
        retry = true;
      }
    } else {
      mFailedSeconds = 0;
    }
  }
  if (retry) {
    mShared->mWake.notify_all();
  }
}

Render::Readiness DocumentSource::Prepare() {
  Shown next;
  {
    std::scoped_lock lock(mShared->mMutex);
    if (mShared->mVersion == mSeenVersion) {
      return Render::Readiness::Unchanged;
    }
    mSeenVersion = mShared->mVersion;
    const auto& status = mShared->mStatus;
    const auto& view = mShared->mView;
    next.mPage = ClampPage(view.mPage, status.mPageCount);
    if (status.mState == DocumentState::Failed) {
      next.mMessage = StringUtil::Widen(std::format("Can't show {}\n{}", StringUtil::Narrow(mOptions.mName), status.mProblem));
    } else if (status.mState == DocumentState::Loading) {
      next.mMessage = std::format(L"Opening {}...", mOptions.mName);
    } else if (auto image = mShared->mCache.Get(next.mPage, view.mWidth)) {
      next.mImage = std::move(image);
    } else if (const auto problem = mShared->mPageProblems.find(next.mPage); problem != mShared->mPageProblems.end()) {
      next.mMessage = StringUtil::Widen(std::format("Page {} of {}\n{}", next.mPage + 1, status.mPageCount, problem->second));
    } else if (mShown.mImage) {
      return Render::Readiness::Unchanged;// being drawn: the last page stays up meanwhile
    } else {
      next.mMessage = std::format(L"Page {} of {}...", next.mPage + 1, status.mPageCount);
    }
  }
  if (next.mImage == mShown.mImage && next.mMessage == mShown.mMessage && next.mPage == mShown.mPage) {
    return Render::Readiness::Unchanged;
  }
  mShown = std::move(next);
  return Render::Readiness::Ready;
}

Render::PixelSize DocumentSource::Size() const {
  if (mShown.mImage) {
    return {mShown.mImage->mWidth, mShown.mImage->mHeight};
  }
  // A message, in Letter's shape, until there's a page
  uint32_t width = 0;
  {
    std::scoped_lock lock(mShared->mMutex);
    width = mShared->mView.mWidth;
  }
  const auto size = DrawSizeFor(PlaceholderAspect, std::min(width, 1024u));
  return {size.mWidth, size.mHeight};
}

void DocumentSource::CreateResources(ID2D1DeviceContext2* context) {
  mBrush = nullptr;
  mTextFormat = nullptr;
  mBitmap = nullptr;// made by the old context's device
  mUploaded = nullptr;
  winrt::check_hresult(context->CreateSolidColorBrush(Rgb(Theme::Text), mBrush.put()));
  winrt::check_hresult(mDWrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
    DWRITE_FONT_STRETCH_NORMAL, 28.0f, L"", mTextFormat.put()));
  mTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  mTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  mTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  mResourcesFor = context;
}

void DocumentSource::Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint) {
  if (mResourcesFor != context || !mBrush) {
    CreateResources(context);
  }
  const D2D1_RECT_F all {0, 0, static_cast<float>(size.mWidth), static_cast<float>(size.mHeight)};
  const auto& image = mShown.mImage;
  if (image && image->mWidth == size.mWidth && image->mHeight == size.mHeight) {
    if (mUploaded != image) {
      const D2D1_SIZE_U pixels {image->mWidth, image->mHeight};
      const UINT32 pitch = image->mWidth * 4;
      if (mBitmap && mBitmap->GetPixelSize().width == pixels.width && mBitmap->GetPixelSize().height == pixels.height) {
        winrt::check_hresult(mBitmap->CopyFromMemory(nullptr, image->mPixels.data(), pitch));
      } else {
        mBitmap = nullptr;
        const auto properties = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(context->CreateBitmap(pixels, image->mPixels.data(), pitch, properties, mBitmap.put()));
      }
      mUploaded = image;
    }
    // White under a page with see-through parts (an image), as viewers do
    context->Clear(D2D1::ColorF(D2D1::ColorF::White));
    context->DrawBitmap(mBitmap.get(), all, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    return;
  }
  context->Clear(Rgb(Theme::Background));
  const auto& message = mShown.mMessage.empty() ? std::wstring {L"..."} : mShown.mMessage;
  const float margin = all.right / 12;
  context->DrawText(message.c_str(), static_cast<UINT32>(message.size()), mTextFormat.get(),
    {margin, margin, all.right - margin, all.bottom - margin}, mBrush.get());
}

// ---- Any thread ----------------------------------------------------------------

std::wstring DocumentSource::Describe() const {
  std::scoped_lock lock(mShared->mMutex);
  const auto& status = mShared->mStatus;
  switch (status.mState) {
    case DocumentState::Loading:
      return L"opening";
    case DocumentState::Failed:
      return StringUtil::Widen("can't show it: " + status.mProblem);
    case DocumentState::Ready:
      break;
  }
  return std::format(L"page {} of {}", ClampPage(mShared->mView.mPage, status.mPageCount) + 1, status.mPageCount);
}

std::string DocumentSource::StatsText() const {
  std::scoped_lock lock(mShared->mMutex);
  const auto& s = *mShared;
  if (s.mDrawn == 0) {
    return {};
  }
  return std::format("pages drawn {} ({} ahead), {:.0f} ms average, {:.0f} ms at most; {} kept, {} MB", s.mDrawn, s.mDrawnAhead,
    s.mDrawMsTotal / static_cast<double>(s.mDrawn), s.mDrawMsMax, s.mCache.Count(), s.mCache.Bytes() >> 20);
}

uint64_t DocumentSource::PagesDrawn() const {
  std::scoped_lock lock(mShared->mMutex);
  return mShared->mDrawn;
}

}// namespace OpenDesktopVR::Documents
