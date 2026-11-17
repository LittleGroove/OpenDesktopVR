// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/ClickSound.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/ThreadUtil.hpp>
#include <OpenDesktopVR/TimeUtil.hpp>

#include <Windows.h>

#include <xaudio2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

namespace OpenDesktopVR::VirtualKeyboard {

namespace {

constexpr uint32_t SampleRate = 48000;
constexpr std::size_t VoiceCount = 4;// clicks may overlap when typing fast

}// namespace

std::vector<int16_t> ClickSound::MakeClick() {
  // About 30 ms: a fast-decaying noise burst for the tick, and two damped
  // tones for a little body. A fixed seed: every click is the same.
  constexpr std::size_t length = SampleRate * 30 / 1000;
  std::vector<float> wave(length);
  uint32_t seed = 0x2545F491u;
  float peak = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(SampleRate);
    seed = (seed * 1664525u) + 1013904223u;
    const float noise = (static_cast<float>(seed >> 8) / static_cast<float>(1u << 24)) * 2.0f - 1.0f;
    const float tick = noise * std::exp(-t / 0.0012f);
    const float high = std::sin(2.0f * std::numbers::pi_v<float> * 2400.0f * t) * std::exp(-t / 0.004f);
    const float low = std::sin(2.0f * std::numbers::pi_v<float> * 850.0f * t) * std::exp(-t / 0.006f);
    wave[i] = (0.55f * tick) + (0.35f * high) + (0.25f * low);
    peak = std::max(peak, std::abs(wave[i]));
  }
  std::vector<int16_t> samples(length);
  const float scale = peak > 0 ? (0.6f * 32767.0f / peak) : 0.0f;
  for (std::size_t i = 0; i < length; ++i) {
    samples[i] = static_cast<int16_t>(std::lround(wave[i] * scale));
  }
  return samples;
}

struct ClickSound::Impl final : IXAudio2EngineCallback {
  std::vector<int16_t> mSamples {MakeClick()};
  HANDLE mWake {CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  HANDLE mStop {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  std::atomic<float> mVolume {0};
  std::atomic<bool> mDeviceLost {false};
  std::thread mThread;

  // The audio thread's own
  IXAudio2* mEngine {nullptr};
  IXAudio2MasteringVoice* mMaster {nullptr};
  std::array<IXAudio2SourceVoice*, VoiceCount> mVoices {};
  std::size_t mNext {0};
  TimeUtil::TimePoint mNextAttempt {};
  bool mFailureLogged {false};

  Impl() {
    mThread = std::thread([this] {
      ThreadUtil::SetCurrentThreadName(L"Key click");
      ThreadUtil::CatchAll("key click thread", [this] { Main(); });
    });
  }

  ~Impl() {
    SetEvent(mStop);
    if (mThread.joinable()) {
      mThread.join();
    }
    CloseHandle(mWake);
    CloseHandle(mStop);
  }

  // IXAudio2EngineCallback: the audio device went (unplugged, changed)
  void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {
  }
  void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {
  }
  void STDMETHODCALLTYPE OnCriticalError(HRESULT) noexcept override {
    mDeviceLost = true;
  }

  void Main() {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HANDLE handles[] {mStop, mWake};
    while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
      ThreadUtil::CatchAll("key click", [this] { PlayNow(mVolume.load()); });
    }
    Release();
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
  }

  void Release() noexcept {
    for (auto& voice: mVoices) {
      if (voice) {
        voice->DestroyVoice();
        voice = nullptr;
      }
    }
    if (mMaster) {
      mMaster->DestroyVoice();
      mMaster = nullptr;
    }
    if (mEngine) {
      mEngine->UnregisterForCallbacks(this);
      mEngine->StopEngine();
      mEngine->Release();
      mEngine = nullptr;
    }
  }

  bool Ensure() {
    if (mDeviceLost.exchange(false)) {
      Log::Info("Key click: the audio device changed; starting again");
      Release();
    }
    if (mEngine) {
      return true;
    }
    const auto now = TimeUtil::Now();
    if (now < mNextAttempt) {
      return false;
    }
    mNextAttempt = now + std::chrono::seconds(10);
    const auto fail = [&](const char* what, HRESULT hr) {
      if (!mFailureLogged) {
        mFailureLogged = true;
        Log::Warning("Key click: {} failed (0x{:08X}); the keyboard is silent", what, static_cast<uint32_t>(hr));
      }
      Release();
      return false;
    };
    HRESULT hr = XAudio2Create(&mEngine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !mEngine) {
      mEngine = nullptr;
      return fail("starting XAudio2", hr);
    }
    mEngine->RegisterForCallbacks(this);
    if (hr = mEngine->CreateMasteringVoice(&mMaster); FAILED(hr)) {
      return fail("opening the audio device", hr);
    }
    WAVEFORMATEX format {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = SampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = SampleRate * 2;
    for (auto& voice: mVoices) {
      if (hr = mEngine->CreateSourceVoice(&voice, &format); FAILED(hr)) {
        return fail("making a voice", hr);
      }
    }
    return true;
  }

  void PlayNow(float volume) {
    if (volume <= 0 || !Ensure()) {
      return;
    }
    auto* voice = mVoices[mNext++ % mVoices.size()];
    voice->Stop();
    voice->FlushSourceBuffers();
    XAUDIO2_BUFFER buffer {};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(mSamples.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(mSamples.data());
    if (SUCCEEDED(voice->SubmitSourceBuffer(&buffer))) {
      voice->SetVolume(std::clamp(volume, 0.0f, 1.0f));
      voice->Start();
    }
  }
};

ClickSound::ClickSound() : mImpl(std::make_unique<Impl>()) {
}

ClickSound::~ClickSound() = default;

void ClickSound::Play(float volume) noexcept {
  if (!std::isfinite(volume) || volume <= 0) {
    return;
  }
  mImpl->mVolume = volume;
  SetEvent(mImpl->mWake);
}

}// namespace OpenDesktopVR::VirtualKeyboard
