// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// A document panel's content (M8a, ADR-047). Its own thread opens the
// document and draws pages into memory; the render thread only uploads the
// page in view and draws it, so a slow page never holds up other panels.
// - The page asked for is drawn first; then, while the panel is in use,
//   the next pages and the one before, so a flip is instant (M8's exit).
//   Only the newest request counts: a quick run of flips never queues.
// - While the next page is being drawn, the last one stays up.
// - A page that can't be drawn, or a document that can't be opened, shows
//   why on the panel (DOC-3). A document that may open later (not there
//   yet, or in use) is tried again every few seconds.
#pragma once

#include <OpenDesktopVR/ContentSource.hpp>
#include <OpenDesktopVR/DocumentStatus.hpp>
#include <OpenDesktopVR/GpuDevice.hpp>
#include <OpenDesktopVR/PageCache.hpp>

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace OpenDesktopVR::Documents {

// Drawn pages kept per panel: about 8 pages of a default panel, the four
// drawn ahead among them (several document panels add up)
inline constexpr std::size_t DefaultCacheBytes = 64u << 20;
// A document that may open later is tried again this often (seconds)
inline constexpr int RetrySeconds = 5;

struct DocumentSourceOptions {
  std::filesystem::path mPath;// a file or a folder
  std::wstring mName;// for the panel and logs
  uint64_t mPanelID {0};
  Render::PageView mView;
  std::shared_ptr<StatusBoard> mBoard;// may be null
  std::size_t mCacheBytes {DefaultCacheBytes};
};

class DocumentSource final : public Render::ContentSource {
 public:
  DocumentSource(Render::GpuDevice& gpu, DocumentSourceOptions options, Render::MarkDirty markDirty);
  ~DocumentSource() override;
  DocumentSource(const DocumentSource&) = delete;
  DocumentSource& operator=(const DocumentSource&) = delete;

  Render::PixelSize Size() const override;
  Render::Readiness Prepare() override;
  void Render(ID2D1DeviceContext2* context, Render::PixelSize size, TimeUtil::TimePoint now) override;
  void SetActive(bool active) override;
  void Maintain(TimeUtil::TimePoint now) override;
  void SetPageView(const Render::PageView& view) override;
  std::wstring Describe() const override;
  std::string StatsText() const override;

  // Tests (render thread): what Prepare chose
  struct Shown {
    std::shared_ptr<const PageImage> mImage;// null: a message instead
    uint32_t mPage {0};
    std::wstring mMessage;
  };
  const Shown& ShownNow() const noexcept {
    return mShown;
  }
  // Tests (any thread): pages drawn so far, and how many were drawn ahead
  uint64_t PagesDrawn() const;

 private:
  struct Shared;
  void Work(std::stop_token stop);
  void CreateResources(ID2D1DeviceContext2* context);

  DocumentSourceOptions mOptions;
  Render::MarkDirty mMarkDirty;
  std::shared_ptr<Shared> mShared;
  // Render thread
  Shown mShown;
  uint64_t mSeenVersion {UINT64_MAX};
  int mFailedSeconds {0};
  winrt::com_ptr<IDWriteFactory> mDWrite;
  winrt::com_ptr<IDWriteTextFormat> mTextFormat;
  winrt::com_ptr<ID2D1SolidColorBrush> mBrush;
  winrt::com_ptr<ID2D1Bitmap1> mBitmap;
  std::shared_ptr<const PageImage> mUploaded;
  ID2D1DeviceContext2* mResourcesFor {nullptr};
  // Last: started once everything above exists, stopped first
  std::jthread mWorker;
};

}// namespace OpenDesktopVR::Documents
