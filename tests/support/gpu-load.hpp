// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// A game-like GPU delay for tests (test round 7). In the game, the host's GPU
// work completed about 44 ms after it was sent, queued behind Elite
// Dangerous's frames. A load on another device doesn't reproduce that on
// the development PC (Windows 11 slips other work in within a millisecond,
// even with the load's priority raised), so this queues ~16 ms batches of
// compute work on the test's own device, keeping about three in flight:
// everything sent after them, a GPU check or a draw, completes that late.
//
// The test that includes this compiles GpuLoad.hlsl into GpuLoadCS.h.
#pragma once

#include <OpenDesktopVR/TimeUtil.hpp>

#include <Windows.h>

#include <d3d11.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <deque>

namespace OpenDesktopVR::Testing {

namespace GpuLoadShaders {
#include <GpuLoadCS.h>
}

class GpuLoad {
 public:
  // Call Pump() often (every few ms) from the thread that owns `context`
  GpuLoad(ID3D11Device* device, ID3D11DeviceContext* context, double batchMs = 16.0, std::size_t queued = 3)
    : mDevice(device), mContext(context), mQueued(queued) {
    const D3D11_TEXTURE2D_DESC desc {
      .Width = 1024, .Height = 1024, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
      .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_UNORDERED_ACCESS};
    winrt::com_ptr<ID3D11Texture2D> texture;
    if (FAILED(mDevice->CreateTexture2D(&desc, nullptr, texture.put()))
        || FAILED(mDevice->CreateUnorderedAccessView(texture.get(), nullptr, mOutput.put()))
        || FAILED(mDevice->CreateComputeShader(
          GpuLoadShaders::g_GpuLoadCS, sizeof(GpuLoadShaders::g_GpuLoadCS), nullptr, mShader.put()))) {
      mShader = nullptr;
      return;
    }
    // Calibrate: how many passes make one batch of `batchMs` (the first
    // dispatch includes warming up, so it's not timed)
    TimeBatch(1);
    int passes = 1;
    double ms = TimeBatch(passes);
    while (ms < batchMs / 2 && passes < 4000) {
      passes *= 2;
      ms = TimeBatch(passes);
    }
    mPasses = std::clamp(static_cast<int>(std::lround(passes * batchMs / std::max(ms, 0.01))), 1, 8000);
    mBatchMs = TimeBatch(mPasses);
  }
  ~GpuLoad() {
    // Let the queued work finish before the caller's resources go
    for (auto& query: mInFlight) {
      Wait(query.get());
    }
  }
  GpuLoad(const GpuLoad&) = delete;
  GpuLoad& operator=(const GpuLoad&) = delete;

  bool IsReady() const noexcept {
    return mShader != nullptr;
  }
  // One batch's GPU time, measured alone
  double BatchMs() const noexcept {
    return mBatchMs;
  }

  // Keeps `queued` batches in flight; never waits
  void Pump() {
    if (!mShader) {
      return;
    }
    while (!mInFlight.empty()) {
      BOOL done = FALSE;
      if (mContext->GetData(mInFlight.front().get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        break;
      }
      mInFlight.pop_front();
    }
    while (mInFlight.size() < mQueued) {
      Submit(mPasses);
      mInFlight.push_back(EndQuery());
      mContext->Flush();
    }
  }

  // How long a trivial piece of work sent now takes to complete: the wait any
  // GPU readback or fence sees
  static double RoundTripMs(ID3D11Device* device, ID3D11DeviceContext* context) {
    const D3D11_QUERY_DESC queryDesc {D3D11_QUERY_EVENT, 0};
    winrt::com_ptr<ID3D11Query> query;
    if (FAILED(device->CreateQuery(&queryDesc, query.put()))) {
      return -1;
    }
    const auto start = TimeUtil::Now();
    context->End(query.get());
    context->Flush();
    if (!Wait(context, query.get())) {
      return -1;
    }
    return TimeUtil::ToMilliseconds(TimeUtil::Now() - start);
  }

 private:
  static bool Wait(ID3D11DeviceContext* context, ID3D11Query* query) {
    const auto start = TimeUtil::Now();
    BOOL done = FALSE;
    while (context->GetData(query, &done, sizeof(done), 0) != S_OK) {
      if (TimeUtil::Now() - start > std::chrono::seconds {2}) {
        return false;
      }
      Sleep(0);
    }
    return true;
  }
  void Wait(ID3D11Query* query) {
    Wait(mContext, query);
  }
  winrt::com_ptr<ID3D11Query> EndQuery() {
    const D3D11_QUERY_DESC queryDesc {D3D11_QUERY_EVENT, 0};
    winrt::com_ptr<ID3D11Query> query;
    mDevice->CreateQuery(&queryDesc, query.put());
    mContext->End(query.get());
    return query;
  }
  void Submit(int passes) {
    ID3D11UnorderedAccessView* uav = mOutput.get();
    mContext->CSSetShader(mShader.get(), nullptr, 0);
    mContext->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    for (int i = 0; i < passes; ++i) {
      mContext->Dispatch(1024 / 8, 1024 / 8, 1);
    }
    ID3D11UnorderedAccessView* none = nullptr;
    mContext->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    mContext->CSSetShader(nullptr, nullptr, 0);
  }
  double TimeBatch(int passes) {
    Submit(passes);
    return RoundTripMs(mDevice, mContext);
  }

  ID3D11Device* mDevice;
  ID3D11DeviceContext* mContext;
  winrt::com_ptr<ID3D11UnorderedAccessView> mOutput;
  winrt::com_ptr<ID3D11ComputeShader> mShader;
  std::deque<winrt::com_ptr<ID3D11Query>> mInFlight;
  std::size_t mQueued {3};
  int mPasses {1};
  double mBatchMs {0};
};

}// namespace OpenDesktopVR::Testing
