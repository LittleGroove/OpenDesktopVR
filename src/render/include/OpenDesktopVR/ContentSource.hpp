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

// A rectangle within the content, in pixels
struct PixelRect {
  uint32_t mLeft {0};
  uint32_t mTop {0};
  uint32_t mWidth {0};
  uint32_t mHeight {0};
  bool operator==(const PixelRect&) const = default;
};

// How many pixels to trim from each edge (CT-12): frameless apps draw their
// own title bar inside the client area. Pixels, not fractions, so a trim
// stays on the title bar when the window is resized (test round 5).
struct Crop {
  // Something is always left: at least this much each way, or all of it if
  // the content is smaller
  static constexpr uint32_t MinimumLeft = 16;
  // Hand-edited files: anything larger is treated as this
  static constexpr uint32_t MaximumEdge = 16384;

  uint32_t mLeft {0};
  uint32_t mTop {0};
  uint32_t mRight {0};
  uint32_t mBottom {0};
  bool operator==(const Crop&) const = default;
  bool IsEmpty() const noexcept {
    return mLeft == 0 && mTop == 0 && mRight == 0 && mBottom == 0;
  }
  // What's left of content of `size`. Trims that would leave less than
  // MinimumLeft shrink in proportion, so a window made small keeps a sliver
  // of the part you chose, not the part you trimmed.
  PixelRect Apply(PixelSize size) const noexcept;
};

// Given to a source when it's made: call it, from any thread, when new
// content exists. It marks the panel dirty and wakes the render loop (at
// most every 8 ms: CP-2).
using MarkDirty = std::function<void()>;

// What Prepare found
enum class Readiness {
  Ready,     // new content to draw
  Unchanged, // what arrived was identical to what's shown: nothing to draw
  Pending,   // still finding out: ask again in a couple of milliseconds
};

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

  // Before drawing a content change, on the render thread: is there really
  // something new? Sources that can't tell say Ready.
  virtual Readiness Prepare() {
    return Readiness::Ready;
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

  // Trim the content's edges; sources that can't crop ignore it
  virtual void SetCrop(const Crop& /*crop*/) {
  }

  // Keep the content's app updating while other windows cover it (CT-3:
  // the app helper, for captured windows); others ignore it. Render thread.
  virtual void SetKeepUpdatingWhenCovered(bool /*keep*/) {
  }

  // The content's size before trimming (a window's client area), for the
  // settings window's trim ranges; 0 x 0 while unknown. Any thread.
  virtual PixelSize UntrimmedSize() const {
    return {};
  }

  // A short state for logs and menus ("capturing", "waiting for Discord")
  virtual std::wstring Describe() const {
    return {};
  }

  // Counters for the 60 s stats line; empty if there's nothing to say
  virtual std::string StatsText() const {
    return {};
  }
};

}// namespace OpenDesktopVR::Render
