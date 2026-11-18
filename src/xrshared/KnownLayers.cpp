// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/KnownLayers.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace OpenDesktopVR::KnownLayers {

namespace {

bool Contains(std::string_view text, std::string_view word) noexcept {
  const auto it = std::search(text.begin(), text.end(), word.begin(), word.end(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
  return it != text.end();
}

struct Word {
  std::string_view mWord;
  Kind mKind;
};

constexpr std::array Words {
  Word {"ultraleap", Ultraleap},
  Word {"leapmotion", Ultraleap},
  Word {"neck", PoseChanging},// XRNeckSafer
  Word {"motion_compensation", PoseChanging},
  Word {"motioncompensation", PoseChanging},
  Word {"motion-compensation", PoseChanging},
  Word {"obs_mirror", MirrorCapture},
  Word {"obsmirror", MirrorCapture},
  Word {"mirror", MirrorCapture},
  Word {"capture", MirrorCapture},
};

}// namespace

Kind Classify(std::string_view nameOrPath) noexcept {
  for (const auto& word: Words) {
    if (Contains(nameOrPath, word.mWord)) {
      return word.mKind;
    }
  }
  return None;
}

std::string_view Describe(Kind kind) noexcept {
  switch (kind) {
    case Ultraleap:
      return "Ultraleap hand tracking";
    case PoseChanging:
      return "a pose-changing layer (neck safety or motion compensation)";
    case MirrorCapture:
      return "a mirror or capture layer";
    default:
      return "another layer";
  }
}

}// namespace OpenDesktopVR::KnownLayers
