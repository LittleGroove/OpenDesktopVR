// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// What a panel shows. Each kind of content (test pattern, captured window;
// documents and the keyboard later) implements this, and the render loop
// treats them all the same (doc 04 "content source").
#pragma once

#include <OpenDesktopVR/TimeUtil.hpp>

#include <d2d1_3.h>

#include <cstdint>
#include <functional>
#include <string>

namespace OpenDesktopVR::Render {

struct PixelSize {
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  bool operator==(const PixelSize&) const = default;
};

// Given to a source when it's made: call it, from any thread, when new
// content exists. It marks the panel dirty and wakes the render loop (at
// most every 8 ms: CP-2).
using MarkDirty = std::function<void()>;

class ContentSource {
 public:
  virtual ~ContentSource() = default;

  // The texture size this content wants right now
  virtual PixelSize Size() const = 0;

  // True if the content changes on its own without telling us (an
  // animation): the render loop keeps it dirty, and the panel's cap sets the
  // pace. Sources that report changes through MarkDirty return false.
  virtual bool IsAnimated() const {
    return false;
  }

  // Draws one frame. The context is between BeginDraw and EndDraw. Called on
  // the render thread only.
  virtual void Render(ID2D1DeviceContext2* context, PixelSize size, TimeUtil::TimePoint now) = 0;

  // Hidden panels and panels nobody is watching: stop producing frames, but
  // keep everything needed to resume at once (CP-5)
  virtual void SetActive(bool /*active*/) {
  }

  // Called on the render thread about once a second: re-find a window,
  // retry a failed start (CP-6, CP-7)
  virtual void Maintain(TimeUtil::TimePoint /*now*/) {
  }

  // A short state for logs and menus ("capturing", "waiting for Discord")
  virtual std::wstring Describe() const {
    return {};
  }
};

}// namespace OpenDesktopVR::Render
