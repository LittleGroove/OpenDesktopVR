// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// What a panel shows. Each kind of content (test pattern now; captured
// windows, documents and the keyboard later) implements this, and the render
// loop treats them all the same.
#pragma once

#include <OpenDesktopVR/TimeUtil.hpp>

#include <d2d1_3.h>

#include <cstdint>

namespace OpenDesktopVR::Render {

struct PixelSize {
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  bool operator==(const PixelSize&) const = default;
};

class ContentSource {
 public:
  virtual ~ContentSource() = default;

  // The texture size this content wants
  virtual PixelSize Size() const = 0;

  // True if the content changes on its own (video, an animation): the render
  // loop then keeps it dirty, and the panel's update cap sets the pace
  virtual bool IsAnimated() const {
    return false;
  }

  // Draws one frame. The target is cleared by the caller; the context is
  // between BeginDraw and EndDraw. Called on the render thread only.
  virtual void Render(ID2D1DeviceContext2* context, PixelSize size, TimeUtil::TimePoint now) = 0;
};

}// namespace OpenDesktopVR::Render
