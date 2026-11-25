// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// A document a panel shows (M8a, ADR-047): one PDF, one image, or a folder
// of them as one run of pages. Used on its source's own thread only:
// - every file is read whole into memory first, so the user's file is
//   never held open (DOC-7: a private copy) and can change under us;
// - PDFs through Windows' own reader (Windows.Data.Pdf), images through
//   Windows' decoders (Windows.Graphics.Imaging), both drawn to memory;
// - the PDF objects are released on that thread, never on the UI or the
//   render thread (DOC-7: their release can pump messages, reported);
// - a page that can't be drawn says why, for a placeholder (DOC-3).
#pragma once

#include <OpenDesktopVR/DocumentFormats.hpp>
#include <OpenDesktopVR/PageCache.hpp>

#include <winrt/Windows.Foundation.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace OpenDesktopVR::Documents {

// Lets another thread stop what the document thread waits on (a page being
// drawn), and anything it starts afterwards: a panel being removed never
// waits for a slow page
class Canceller {
 public:
  // Any thread
  void Cancel() noexcept;
  bool Cancelled() const noexcept;

  // Waits for a WinRT operation; throws winrt::hresult_canceled once
  // cancelled
  template <class Async>
  auto Wait(const Async& async) {
    Begin(async);
    struct Ending {
      Canceller& mCanceller;
      ~Ending() {
        mCanceller.End();
      }
    } ending {*this};
    return async.get();
  }

 private:
  void Begin(const winrt::Windows::Foundation::IAsyncInfo& info);
  void End() noexcept;

  mutable std::mutex mMutex;
  winrt::Windows::Foundation::IAsyncInfo mRunning {nullptr};
  bool mCancelled {false};
};

// Why a document can't be opened, in the user's words
struct Problem {
  std::string mText;
  bool mRetry {false};// it may open later (not there yet, or in use)
};

class Document {
 public:
  // A PDF, an image, or a folder of them
  static std::expected<std::unique_ptr<Document>, Problem> Open(const std::filesystem::path& path, Canceller& canceller);
  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  uint32_t PageCount() const noexcept;
  // Draws a page (from 0) `width` pixels wide (DrawSizeFor); or why it
  // can't (DOC-3)
  std::expected<PageImage, std::string> Draw(uint32_t page, uint32_t width, Canceller& canceller);

 private:
  Document();
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

}// namespace OpenDesktopVR::Documents
