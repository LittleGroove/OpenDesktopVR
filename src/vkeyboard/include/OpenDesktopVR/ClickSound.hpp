// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The virtual keyboard's key click (K11; ADR-036): our own sound, made
// when it starts (a short, soft tick), played through XAudio2 on the
// default audio device.
//
// Play() never waits: it only wakes this object's own thread, which does
// all the audio work, including starting XAudio2 and starting again after
// the audio device changes (a headset plugged in, Virtual Desktop's audio).
// A failure is logged once and the keyboard stays silent.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace OpenDesktopVR::VirtualKeyboard {

class ClickSound {
 public:
  ClickSound();
  ~ClickSound();
  ClickSound(const ClickSound&) = delete;
  ClickSound& operator=(const ClickSound&) = delete;

  // Any thread; `volume` 0 to 1 (0: nothing)
  void Play(float volume) noexcept;

  // The click's samples (48 kHz, mono, 16-bit) at full volume; for tests
  static std::vector<int16_t> MakeClick();

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

}// namespace OpenDesktopVR::VirtualKeyboard
