// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Drawn pages, kept in memory so the next page is ready before it's asked
// for (doc 04: "cache rendered pages, LRU, bounded by memory"; M8's exit: a
// 300-page PDF flips with no visible stall). Each page is kept at the width
// it was drawn at: a panel made wider draws its pages again.
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <vector>

namespace OpenDesktopVR::Documents {

// A drawn page: premultiplied BGRA, rows of mWidth * 4 bytes
struct PageImage {
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  std::vector<uint8_t> mPixels;
  std::size_t Bytes() const noexcept {
    return mPixels.size();
  }
};

class PageCache {
 public:
  explicit PageCache(std::size_t budgetBytes) noexcept : mBudget(budgetBytes) {
  }

  // The page at that width, now the most recently used; null if absent
  std::shared_ptr<const PageImage> Get(uint32_t page, uint32_t width);
  bool Has(uint32_t page, uint32_t width) const noexcept;
  // Adds or replaces it, then drops the least recently used pages until
  // the rest fit the budget. The page `keep` (the one in view) is never
  // dropped, at any width, even if it alone is over the budget.
  void Put(uint32_t page, uint32_t width, std::shared_ptr<const PageImage> image, uint32_t keep);
  void Clear() noexcept;

  std::size_t Bytes() const noexcept {
    return mBytes;
  }
  std::size_t Count() const noexcept {
    return mEntries.size();
  }

 private:
  struct Entry {
    uint32_t mPage {0};
    uint32_t mWidth {0};
    std::shared_ptr<const PageImage> mImage;
  };
  std::list<Entry> mEntries;// most recently used first
  std::size_t mBudget {0};
  std::size_t mBytes {0};
};

}// namespace OpenDesktopVR::Documents
