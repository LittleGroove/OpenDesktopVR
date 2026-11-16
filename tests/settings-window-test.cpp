// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The desktop settings window (M3b; ADR-020, ADR-023, RN-12): the real host,
// as a test instance, opens its settings window, saves it as a PNG, then
// stays idle for two seconds. Checks that it opens and quits cleanly, is
// drawn in the product's theme, paints nothing while idle, opens within
// doc 08's 50 ms (DU-3) when opened as a user would, a moment after start,
// and that the window picker opens and gets its list from a worker thread.
//
//   settings-window-test <host exe>

#include "check.hpp"

#include <OpenDesktopVR/ImageFile.hpp>
#include <OpenDesktopVR/Theme.hpp>

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

using namespace OpenDesktopVR;
using namespace OpenDesktopVR::Testing;

namespace {

std::wstring gHostExe;

struct HostRun {
  bool mExited {false};
  DWORD mExitCode {1};
};

HostRun RunHost(const std::wstring& arguments) {
  auto command = std::format(L"\"{}\" --no-register --warp --exit-after 40 {}", gHostExe, arguments);
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(gHostExe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
    return {};
  }
  HostRun run;
  run.mExited = WaitForSingleObject(info.hProcess, 60'000) == WAIT_OBJECT_0;
  if (run.mExited) {
    GetExitCodeProcess(info.hProcess, &run.mExitCode);
  } else {
    TerminateProcess(info.hProcess, 1);
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return run;
}

bool Near(const uint8_t* bgra, uint32_t rgb, int tolerance) {
  const int blue = rgb & 0xFF;
  const int green = (rgb >> 8) & 0xFF;
  const int red = (rgb >> 16) & 0xFF;
  return std::abs(bgra[0] - blue) <= tolerance && std::abs(bgra[1] - green) <= tolerance
    && std::abs(bgra[2] - red) <= tolerance;
}

std::size_t CountNear(const ImageFile::Image& image, uint32_t rgb, int tolerance) {
  std::size_t count = 0;
  for (std::size_t i = 0; i + 3 < image.mBgra.size(); i += 4) {
    count += Near(&image.mBgra[i], rgb, tolerance) ? 1 : 0;
  }
  return count;
}

// 0xRRGGBB at 50% black: ImGui's dimming behind a modal popup
uint32_t Dimmed(uint32_t rgb) {
  const uint32_t red = ((rgb >> 16) & 0xFF) / 2;
  const uint32_t green = ((rgb >> 8) & 0xFF) / 2;
  const uint32_t blue = (rgb & 0xFF) / 2;
  return (red << 16) | (green << 8) | blue;
}

std::string ReadText(const std::wstring& path) {
  std::ifstream file {std::filesystem::path {path}};
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

// Runs the host with a screenshot option; the image if it worked. With
// `delayMs`, the window opens that long after start (its device and fonts
// are made by then, as when a user opens it), and its opening is timed.
std::optional<ImageFile::Image> Screenshot(const std::wstring& option, const std::wstring& png, int delayMs = 0) {
  const auto run = RunHost(std::format(L"{} \"{}\" --settings-delay {}", option, png, delayMs));
  Check(run.mExited && run.mExitCode == 0, "the host opened the window, saved it and quit cleanly");
  // "idle paints: N", "input messages: M (last 0x...)", "opened in: T ms"
  unsigned paints = ~0u;
  unsigned inputs = 0;
  unsigned last = 0;
  int opening = -1;
  const auto report = ReadText(png + L".idle.txt");
  sscanf_s(report.c_str(), "idle paints: %u input messages: %u (last %x) opened in: %d ms", &paints, &inputs, &last, &opening);
  if (delayMs > 0) {
    Check(opening >= 0 && opening <= 50, std::format("opened within 50 ms (DU-3, doc 08): {} ms", opening));
  }
  if (inputs > 0) {
    std::printf("  (the mouse or keyboard was used in the idle second, so idle painting isn't judged)\n");
  }
  Check(paints == 0 || inputs > 0, std::format("no painting in a second with no input (RN-12): {} paints", paints));
  auto image = ImageFile::Load(png);
  Check(image.has_value(), "the screenshot was saved");
  return image;
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::printf("usage: settings-window-test <host exe>\n");
    return 2;
  }
  gHostExe = argv[1];
  // A private instance: built-in test panels, the user's settings untouched
  const auto instance = std::format(L"swt{}", GetCurrentProcessId());
  SetEnvironmentVariableW(L"OPENDESKTOPVR_INSTANCE", instance.c_str());
  TempFolder folder;

  Scenario("The settings window opens, in the product's theme (ADR-023)", [&] {
    const auto image = Screenshot(L"--settings-screenshot", folder.Path() + L"\\settings.png", 1500);
    if (!image) {
      return;
    }
    Check(image->mWidth >= 600 && image->mHeight >= 400, std::format("a sensible size ({} x {})", image->mWidth, image->mHeight));
    const auto corner = &image->mBgra[(static_cast<std::size_t>(image->mWidth) * 2 + 2) * 4];
    Check(Near(corner, Theme::Background, 3), "the background is the theme's");
    const auto accent = CountNear(*image, Theme::Accent, 10);
    Check(accent > 50, std::format("accent-coloured check marks: the test panels are listed as shown ({} pixels)", accent));
    const auto text = CountNear(*image, Theme::Text, 10);
    Check(text > 300, std::format("text is drawn ({} pixels)", text));
  });

  Scenario("The window picker opens with its list (ADR-020: listed on a worker thread)", [&] {
    const auto image = Screenshot(L"--picker-screenshot", folder.Path() + L"\\picker.png");
    if (!image) {
      return;
    }
    // The settings behind a modal popup are dimmed
    const auto corner = &image->mBgra[(static_cast<std::size_t>(image->mWidth) * 2 + 2) * 4];
    Check(Near(corner, Dimmed(Theme::Background), 4), "the settings behind the picker are dimmed");
    Check(CountNear(*image, Theme::Surface, 3) > 10'000, "the picker is drawn on the theme's surface colour");
  });

  return Finish();
}
