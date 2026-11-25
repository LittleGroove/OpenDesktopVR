// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/PageCache.hpp>

#include <algorithm>

namespace OpenDesktopVR::Documents {

std::shared_ptr<const PageImage> PageCache::Get(uint32_t page, uint32_t width) {
  const auto found = std::ranges::find_if(mEntries, [&](const Entry& e) { return e.mPage == page && e.mWidth == width; });
  if (found == mEntries.end()) {
    return nullptr;
  }
  mEntries.splice(mEntries.begin(), mEntries, found);
  return mEntries.front().mImage;
}

bool PageCache::Has(uint32_t page, uint32_t width) const noexcept {
  return std::ranges::any_of(mEntries, [&](const Entry& e) { return e.mPage == page && e.mWidth == width; });
}

void PageCache::Put(uint32_t page, uint32_t width, std::shared_ptr<const PageImage> image, uint32_t keep) {
  if (!image) {
    return;
  }
  if (const auto found = std::ranges::find_if(mEntries, [&](const Entry& e) { return e.mPage == page && e.mWidth == width; });
      found != mEntries.end()) {
    mBytes -= found->mImage->Bytes();
    mEntries.erase(found);
  }
  mBytes += image->Bytes();
  mEntries.push_front({page, width, std::move(image)});
  // From the least recently used end, skipping the page in view
  for (auto it = mEntries.end(); mBytes > mBudget && it != mEntries.begin();) {
    --it;
    if (it->mPage == keep) {
      continue;
    }
    mBytes -= it->mImage->Bytes();
    it = mEntries.erase(it);
  }
}

void PageCache::Clear() noexcept {
  mEntries.clear();
  mBytes = 0;
}

}// namespace OpenDesktopVR::Documents
