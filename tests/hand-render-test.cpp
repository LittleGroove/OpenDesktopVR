// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// Draws synthetic hands with the in-game hand renderer, and saves them as PNGs
// so the look can be checked without a headset. Also measures GPU time.
//
// Usage: hand-render-test [output folder]

#include "synthetic-hands.hpp"

#include <OpenDesktopVR/D3D11/HandRenderer.hpp>

#include <winrt/base.h>

#include <Windows.h>

#include <d3d11.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::SyntheticHands;
using Eye = D3D11::HandRenderer::Eye;
using DirectX::SimpleMath::Matrix;
using DirectX::SimpleMath::Vector2;

namespace {

// Same placement as the interaction tests: a 30x20cm panel, 50cm ahead
const Vector3 PanelCenter {0, 1.0f, -0.5f};
const Vector2 PanelSize {0.30f, 0.20f};

Eye LookAt(const Vector3& position, const Vector3& target, float tanHalfFov) {
  Vector3 forward = target - position;
  forward.Normalize();
  Vector3 right = forward.Cross(Vector3::UnitY);
  right.Normalize();
  const Vector3 up = right.Cross(forward);

  // The eye looks along its local -Z. Try both matrix conventions, and keep
  // whichever maps -Z to 'forward'.
  const Matrix rows(right, up, -forward);
  Quaternion orientation = Quaternion::CreateFromRotationMatrix(rows);
  if (Vector3::Transform(-Vector3::UnitZ, orientation).Dot(forward) < 0.99f) {
    orientation = Quaternion::CreateFromRotationMatrix(rows.Transpose());
  }
  return {
    .mPosition = position,
    .mOrientation = orientation,
    .mTanLeft = -tanHalfFov,
    .mTanRight = tanHalfFov,
    .mTanUp = tanHalfFov,
    .mTanDown = -tanHalfFov,
  };
}

std::optional<Vector2> ToPixel(
  const Eye& eye,
  uint32_t width,
  uint32_t height,
  const Vector3& point) {
  Quaternion inverse;
  eye.mOrientation.Inverse(inverse);
  const Vector3 v = Vector3::Transform(point - eye.mPosition, inverse);
  if (-v.z < 0.01f) {
    return std::nullopt;
  }
  const float tanX = v.x / -v.z;
  const float tanY = v.y / -v.z;
  return Vector2 {
    ((tanX - eye.mTanLeft) / (eye.mTanRight - eye.mTanLeft)) * width,
    ((eye.mTanUp - tanY) / (eye.mTanUp - eye.mTanDown)) * height,
  };
}

void SavePNG(
  const std::filesystem::path& path,
  uint32_t width,
  uint32_t height,
  const std::vector<uint8_t>& bgra) {
  winrt::com_ptr<IWICImagingFactory> factory;
  winrt::check_hresult(CoCreateInstance(
    CLSID_WICImagingFactory,
    nullptr,
    CLSCTX_INPROC_SERVER,
    IID_PPV_ARGS(factory.put())));
  winrt::com_ptr<IWICStream> stream;
  winrt::check_hresult(factory->CreateStream(stream.put()));
  winrt::check_hresult(
    stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE));
  winrt::com_ptr<IWICBitmapEncoder> encoder;
  winrt::check_hresult(
    factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
  winrt::check_hresult(
    encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
  winrt::com_ptr<IWICBitmapFrameEncode> frame;
  winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
  winrt::check_hresult(frame->Initialize(nullptr));
  winrt::check_hresult(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  winrt::check_hresult(frame->SetPixelFormat(&format));
  winrt::check_hresult(frame->WritePixels(
    height,
    width * 4,
    static_cast<UINT>(bgra.size()),
    const_cast<BYTE*>(bgra.data())));
  winrt::check_hresult(frame->Commit());
  winrt::check_hresult(encoder->Commit());
}

class TestRenderer {
 public:
  TestRenderer(uint32_t width, uint32_t height)
    : mWidth(width),
      mHeight(height) {
    for (const auto driver: {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
      if (SUCCEEDED(D3D11CreateDevice(
            nullptr,
            driver,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            mDevice.put(),
            nullptr,
            mContext.put()))) {
        std::printf(
          "Using the %s D3D11 driver\n",
          driver == D3D_DRIVER_TYPE_HARDWARE ? "hardware" : "WARP (software)");
        break;
      }
    }
    if (!mDevice) {
      throw std::runtime_error("Failed to create a D3D11 device");
    }

    D3D11_TEXTURE2D_DESC desc {
      .Width = width,
      .Height = height,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
      .SampleDesc = {1, 0},
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = D3D11_BIND_RENDER_TARGET,
    };
    winrt::check_hresult(
      mDevice->CreateTexture2D(&desc, nullptr, mTexture.put()));
    winrt::check_hresult(mDevice->CreateRenderTargetView(
      mTexture.get(), nullptr, mRenderTargetView.put()));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(
      mDevice->CreateTexture2D(&desc, nullptr, mStaging.put()));

    mRenderer = std::make_unique<D3D11::HandRenderer>(mDevice.get());
  }

  void Draw(const Eye& eye, std::span<const RenderHand> hands) {
    const float transparent[4] {0, 0, 0, 0};
    mContext->ClearRenderTargetView(mRenderTargetView.get(), transparent);
    const D3D11_VIEWPORT viewport {
      .TopLeftX = 0,
      .TopLeftY = 0,
      .Width = static_cast<float>(mWidth),
      .Height = static_cast<float>(mHeight),
      .MinDepth = 0,
      .MaxDepth = 1,
    };
    mRenderer->Render(
      mContext.get(), mRenderTargetView.get(), viewport, eye, hands);
  }

  // Draws the hands over a dark background with a light panel, and
  // saves the result
  void Save(
    const std::filesystem::path& path,
    const Eye& eye,
    std::span<const RenderHand> hands) {
    Draw(eye, hands);
    mContext->CopyResource(mStaging.get(), mTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    winrt::check_hresult(
      mContext->Map(mStaging.get(), 0, D3D11_MAP_READ, 0, &mapped));

    // The panel, projected into the image
    std::array<Vector2, 4> corners {};
    bool panelVisible = true;
    const std::array<Vector2, 4> offsets {{
      {-0.5f, 0.5f},
      {0.5f, 0.5f},
      {0.5f, -0.5f},
      {-0.5f, -0.5f},
    }};
    for (std::size_t i = 0; i < 4; ++i) {
      const auto pixel = ToPixel(
        eye,
        mWidth,
        mHeight,
        PanelCenter
          + Vector3 {
            offsets[i].x * PanelSize.x, offsets[i].y * PanelSize.y, 0});
      if (!pixel) {
        panelVisible = false;
        break;
      }
      corners[i] = *pixel;
    }
    const auto insidePanel = [&](float x, float y) {
      if (!panelVisible) {
        return false;
      }
      bool positive = false;
      bool negative = false;
      for (std::size_t i = 0; i < 4; ++i) {
        const auto& a = corners[i];
        const auto& b = corners[(i + 1) % 4];
        const float cross = ((b.x - a.x) * (y - a.y)) - ((b.y - a.y) * (x - a.x));
        positive |= cross > 0;
        negative |= cross < 0;
      }
      return !(positive && negative);
    };

    std::vector<uint8_t> bgra(mWidth * mHeight * 4);
    for (uint32_t y = 0; y < mHeight; ++y) {
      const auto row = static_cast<const uint8_t*>(mapped.pData)
        + (static_cast<std::size_t>(y) * mapped.RowPitch);
      const float t = static_cast<float>(y) / mHeight;
      for (uint32_t x = 0; x < mWidth; ++x) {
        // Background: dark, like a cockpit at night; the panel is a light page
        float bg[3] {
          40 - (22 * t),
          46 - (26 * t),
          58 - (32 * t),
        };
        if (insidePanel(x + 0.5f, y + 0.5f)) {
          bg[0] = 228;
          bg[1] = 230;
          bg[2] = 234;
        }
        const uint8_t* src = row + (x * 4);
        const float alpha = src[3] / 255.0f;
        uint8_t* dst = &bgra[(static_cast<std::size_t>(y) * mWidth + x) * 4];
        // Premultiplied: out = hand + background * (1 - alpha)
        const auto blend = [&](int channel, float background) {
          return static_cast<uint8_t>(std::clamp(
            src[channel] + (background * (1 - alpha)), 0.0f, 255.0f));
        };
        dst[0] = blend(2, bg[2]);
        dst[1] = blend(1, bg[1]);
        dst[2] = blend(0, bg[0]);
        dst[3] = 255;
      }
    }
    mContext->Unmap(mStaging.get(), 0);
    SavePNG(path, mWidth, mHeight, bgra);
    std::printf("Saved %s\n", path.string().c_str());
  }

  // Average GPU time to clear and draw, in milliseconds
  double MeasureMilliseconds(
    const Eye& eye,
    std::span<const RenderHand> hands,
    int iterations) {
    D3D11_QUERY_DESC disjointDesc {.Query = D3D11_QUERY_TIMESTAMP_DISJOINT};
    D3D11_QUERY_DESC timestampDesc {.Query = D3D11_QUERY_TIMESTAMP};
    winrt::com_ptr<ID3D11Query> disjoint, start, end;
    winrt::check_hresult(mDevice->CreateQuery(&disjointDesc, disjoint.put()));
    winrt::check_hresult(mDevice->CreateQuery(&timestampDesc, start.put()));
    winrt::check_hresult(mDevice->CreateQuery(&timestampDesc, end.put()));

    // Warm up
    Draw(eye, hands);
    mContext->Flush();

    mContext->Begin(disjoint.get());
    mContext->End(start.get());
    for (int i = 0; i < iterations; ++i) {
      Draw(eye, hands);
    }
    mContext->End(end.get());
    mContext->End(disjoint.get());

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData {};
    while (mContext->GetData(
             disjoint.get(), &disjointData, sizeof(disjointData), 0)
           == S_FALSE) {
      Sleep(1);
    }
    uint64_t startTime {}, endTime {};
    while (mContext->GetData(start.get(), &startTime, sizeof(startTime), 0)
           == S_FALSE) {
      Sleep(1);
    }
    while (mContext->GetData(end.get(), &endTime, sizeof(endTime), 0)
           == S_FALSE) {
      Sleep(1);
    }
    if (disjointData.Disjoint || disjointData.Frequency == 0) {
      return -1;
    }
    return (static_cast<double>(endTime - startTime) * 1000.0
            / static_cast<double>(disjointData.Frequency))
      / iterations;
  }

 private:
  uint32_t mWidth {};
  uint32_t mHeight {};
  winrt::com_ptr<ID3D11Device> mDevice;
  winrt::com_ptr<ID3D11DeviceContext> mContext;
  winrt::com_ptr<ID3D11Texture2D> mTexture;
  winrt::com_ptr<ID3D11Texture2D> mStaging;
  winrt::com_ptr<ID3D11RenderTargetView> mRenderTargetView;
  std::unique_ptr<D3D11::HandRenderer> mRenderer;
};

RenderHand Visible(const HandJoints& joints, float visibility = 1.0f) {
  return {.mVisibility = visibility, .mJoints = joints};
}

}// namespace

int main(int argc, char** argv) {
  winrt::check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  const std::filesystem::path outputDir
    = (argc > 1) ? std::filesystem::path {argv[1]}
                 : std::filesystem::current_path();
  std::filesystem::create_directories(outputDir);

  try {
    TestRenderer renderer(1024, 1024);

    // Roughly where your eyes are, looking down at a panel 50cm ahead
    const auto userEye = LookAt({0, 1.25f, 0.05f}, {0, 1.02f, -0.47f}, 0.8f);
    const auto sideEye = LookAt({0.40f, 1.10f, -0.35f}, {0.03f, 1.02f, -0.43f}, 0.6f);

    const auto pointing = MakeHand(Pose::Point, PokeBasis, {0.02f, 1.0f, -0.49f});
    const auto flat = MakeHand(Pose::Flat, PokeBasis, {0.0f, 1.0f, -0.46f});
    const auto leftPointing
      = MakeHand(Pose::Point, LeftPokeBasis, {-0.06f, 1.02f, -0.47f});

    {
      const std::array hands {RenderHand {}, Visible(pointing)};
      renderer.Save(outputDir / "hand-pointing.png", userEye, hands);
      renderer.Save(outputDir / "hand-pointing-side.png", sideEye, hands);
      // Zoomed in on the fingertip and the panel
      const auto closeUp = LookAt({0, 1.25f, 0.05f}, {0.02f, 1.0f, -0.42f}, 0.3f);
      renderer.Save(outputDir / "hand-pointing-closeup.png", closeUp, hands);
    }
    {
      const std::array hands {RenderHand {}, Visible(flat)};
      renderer.Save(outputDir / "hand-flat.png", userEye, hands);
    }
    {
      const std::array hands {RenderHand {}, Visible(pointing, 0.5f)};
      renderer.Save(outputDir / "hand-pointing-fading.png", userEye, hands);
    }
    {
      const std::array hands {Visible(leftPointing), Visible(pointing)};
      renderer.Save(outputDir / "hands-both.png", userEye, hands);
    }

    // Performance, at the in-game size: 60% of a Quest 3's typical per-eye
    // resolution (2064x2208). The side view fills more of the screen, so it's
    // the worse case.
    {
      TestRenderer perf(1238, 1325);
      const std::array hands {RenderHand {}, Visible(pointing)};
      for (const auto& [name, eye]: {
             std::pair {"your view", userEye},
             std::pair {"close side view (worst case)", sideEye},
           }) {
        const auto ms = perf.MeasureMilliseconds(eye, hands, 200);
        std::printf(
          "GPU time, one hand, %s: %.3f ms per eye, about %.2f ms per frame\n",
          name,
          ms,
          ms * 2);
      }
    }
  } catch (const winrt::hresult_error& e) {
    std::printf(
      "Error: %08x %ls\n",
      static_cast<uint32_t>(e.code().value),
      e.message().c_str());
    return 1;
  } catch (const std::exception& e) {
    std::printf("Error: %s\n", e.what());
    return 1;
  }
  return 0;
}
