// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/Theme.hpp>
#include <OpenDesktopVR/UiTheme.hpp>

#include <Windows.h>

#include <ShlObj.h>
#include <imgui.h>

#include <filesystem>
#include <string>

namespace OpenDesktopVR::UiTheme {

namespace {

ImVec4 Vec(uint32_t rgb, float alpha = 1.0f) {
  const auto c = Theme::ToRgba(rgb, alpha);
  return {c.r, c.g, c.b, c.a};
}

std::filesystem::path FontsFolder() {
  PWSTR raw = nullptr;
  std::filesystem::path folder;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &raw)) && raw) {
    folder = raw;
  }
  CoTaskMemFree(raw);
  return folder;
}

}// namespace

unsigned int Color(unsigned int rgb, float alpha) noexcept {
  return ImGui::ColorConvertFloat4ToU32(Vec(rgb, alpha));
}

void Apply(float dpiScale) {
  auto& style = ImGui::GetStyle();
  style = ImGuiStyle {};
  style.WindowRounding = 8;
  style.ChildRounding = 6;
  style.FrameRounding = 6;
  style.PopupRounding = 6;
  style.GrabRounding = 6;
  style.ScrollbarRounding = 6;
  style.WindowPadding = {16, 14};
  style.FramePadding = {10, 6};
  style.ItemSpacing = {10, 8};
  style.WindowBorderSize = 0;
  style.FrameBorderSize = 0;
  style.ScaleAllSizes(dpiScale);

  auto* c = style.Colors;
  c[ImGuiCol_Text] = Vec(Theme::Text);
  c[ImGuiCol_TextDisabled] = Vec(Theme::TextDim);
  c[ImGuiCol_WindowBg] = Vec(Theme::Background);
  c[ImGuiCol_ChildBg] = Vec(Theme::Surface);
  c[ImGuiCol_PopupBg] = Vec(Theme::Surface);
  c[ImGuiCol_Border] = Vec(Theme::Line);
  c[ImGuiCol_FrameBg] = Vec(Theme::SurfaceHigh);
  c[ImGuiCol_FrameBgHovered] = Vec(Theme::Line);
  c[ImGuiCol_FrameBgActive] = Vec(Theme::Line);
  c[ImGuiCol_TitleBg] = Vec(Theme::Background);
  c[ImGuiCol_TitleBgActive] = Vec(Theme::Background);
  c[ImGuiCol_ScrollbarBg] = Vec(Theme::Background);
  c[ImGuiCol_ScrollbarGrab] = Vec(Theme::Line);
  c[ImGuiCol_CheckMark] = Vec(Theme::Accent);
  c[ImGuiCol_SliderGrab] = Vec(Theme::AccentDim);
  c[ImGuiCol_SliderGrabActive] = Vec(Theme::Accent);
  c[ImGuiCol_Button] = Vec(Theme::SurfaceHigh);
  c[ImGuiCol_ButtonHovered] = Vec(Theme::Line);
  c[ImGuiCol_ButtonActive] = Vec(Theme::AccentDim);
  c[ImGuiCol_Header] = Vec(Theme::AccentDim, 0.55f);
  c[ImGuiCol_HeaderHovered] = Vec(Theme::Line);
  c[ImGuiCol_HeaderActive] = Vec(Theme::AccentDim);
  c[ImGuiCol_Separator] = Vec(Theme::Line);
  c[ImGuiCol_TextSelectedBg] = Vec(Theme::Accent, 0.35f);
  c[ImGuiCol_ModalWindowDimBg] = Vec(0x000000, 0.5f);
  c[ImGuiCol_TitleBgCollapsed] = Vec(Theme::Background);
  c[ImGuiCol_ScrollbarGrabHovered] = Vec(Theme::TextDim, 0.6f);
  c[ImGuiCol_ScrollbarGrabActive] = Vec(Theme::AccentDim);
  c[ImGuiCol_SeparatorHovered] = Vec(Theme::AccentDim);
  c[ImGuiCol_SeparatorActive] = Vec(Theme::Accent);
  c[ImGuiCol_ResizeGrip] = Vec(Theme::Line);
  c[ImGuiCol_ResizeGripHovered] = Vec(Theme::AccentDim);
  c[ImGuiCol_ResizeGripActive] = Vec(Theme::Accent);
  c[ImGuiCol_TableHeaderBg] = Vec(Theme::SurfaceHigh);
  c[ImGuiCol_TableBorderStrong] = Vec(Theme::Line);
  c[ImGuiCol_TableBorderLight] = Vec(Theme::Line);
  c[ImGuiCol_TableRowBg] = Vec(Theme::Surface, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = Vec(Theme::SurfaceHigh, 0.35f);
  c[ImGuiCol_NavCursor] = Vec(Theme::Accent);
}

Fonts LoadFonts(float dpiScale) {
  auto& io = ImGui::GetIO();
  io.Fonts->Clear();
  // Window titles come in many scripts: Latin with its extensions, Greek,
  // Cyrillic, and the punctuation and arrows programs put in titles. Others
  // (CJK, emoji) show as '?' for now.
  static constexpr ImWchar Ranges[] = {
    0x0020, 0x024F,// Latin, Latin-1, Latin Extended-A and -B
    0x0370, 0x03FF,// Greek
    0x0400, 0x052F,// Cyrillic
    0x2000, 0x206F,// general punctuation (dashes, quotes, ellipsis)
    0x20AC, 0x20AC,// euro sign
    0x2122, 0x2122,// trade mark
    0x2190, 0x21FF,// arrows
    0x2212, 0x2212,// minus sign
    0,
  };
  Fonts fonts;
  const auto folder = FontsFolder();
  const auto body = folder / L"segoeui.ttf";
  const auto heading = folder / L"seguisb.ttf";
  // ImGui opens font files by UTF-8 name
  const auto utf8 = [](const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string {reinterpret_cast<const char*>(text.data()), text.size()};
  };
  std::error_code error;
  if (!folder.empty() && std::filesystem::exists(body, error)) {
    fonts.mBody = io.Fonts->AddFontFromFileTTF(utf8(body).c_str(), 17.0f * dpiScale, nullptr, Ranges);
    if (std::filesystem::exists(heading, error)) {
      fonts.mHeading = io.Fonts->AddFontFromFileTTF(utf8(heading).c_str(), 24.0f * dpiScale, nullptr, Ranges);
    }
  }
  if (!fonts.mBody) {
    ImFontConfig config;
    config.SizePixels = 15.0f * dpiScale;
    fonts.mBody = io.Fonts->AddFontDefault(&config);
  }
  if (!fonts.mHeading) {
    fonts.mHeading = fonts.mBody;
  }
  // Clear() deleted the old fonts; don't leave ImGui pointing at one
  io.FontDefault = fonts.mBody;
  return fonts;
}

}// namespace OpenDesktopVR::UiTheme
