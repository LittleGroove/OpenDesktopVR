# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Every third-party dependency, pinned by release and SHA-256. Each one is
# listed in THIRD_PARTY_NOTICES. Only permissive licenses (the licensing rules,
# doc 12). FetchContent downloads sources only: SOURCE_SUBDIR points at a
# folder without a CMakeLists.txt, so none of their build scripts run here.
#
# Targets defined here:
#   odvr::simplemath      DirectXTK SimpleMath (MIT), vector and quaternion maths
#   odvr::openxr_headers  Khronos OpenXR SDK headers (Apache-2.0)
#   odvr::json            nlohmann/json (MIT), settings files
#   odvr::cppwinrt        C++/WinRT headers from the Windows SDK (MIT)
#   odvr::imgui           Dear ImGui with its Win32 and Direct3D 11 backends (MIT;
#                         ADR-020: the desktop settings window and in-VR menus)
# and the variable ODVR_FXC (the Windows SDK's shader compiler).

include(FetchContent)

FetchContent_Declare(
  directxtk
  URL "https://github.com/microsoft/DirectXTK/archive/refs/tags/oct2025.tar.gz"
  URL_HASH "SHA256=317ceac23974bb7347cc75851d195e2c54636433bc48d52e4b5f3275c56cf4b1"
  SOURCE_SUBDIR "no-cmake"
)
FetchContent_Declare(
  openxr_sdk
  URL "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.0.25.zip"
  URL_HASH "SHA256=a4d3b8877b2ab1fd0ccddeced8e5f7fe7940cf48793bcc29fb0611c6a53af65e"
  SOURCE_SUBDIR "no-cmake"
)
FetchContent_Declare(
  nlohmann_json
  URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz"
  URL_HASH "SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d"
  SOURCE_SUBDIR "no-cmake"
)
FetchContent_Declare(
  imgui
  URL "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz"
  URL_HASH "SHA256=2aa2d169c569368439e5d5667e0796d09ca5cc6432965ce082e516937d7db254"
  SOURCE_SUBDIR "no-cmake"
)
FetchContent_MakeAvailable(directxtk openxr_sdk nlohmann_json imgui)

# DirectXTK: SimpleMath only
add_library(odvr_simplemath STATIC "${directxtk_SOURCE_DIR}/Src/SimpleMath.cpp")
add_library(odvr::simplemath ALIAS odvr_simplemath)
target_include_directories(
  odvr_simplemath
  PUBLIC "${directxtk_SOURCE_DIR}/Inc"
  PRIVATE "${directxtk_SOURCE_DIR}/Src"
)
set_target_properties(odvr_simplemath PROPERTIES FOLDER "third-party")

add_library(odvr_openxr_headers INTERFACE)
add_library(odvr::openxr_headers ALIAS odvr_openxr_headers)
target_include_directories(
  odvr_openxr_headers SYSTEM
  INTERFACE "${openxr_sdk_SOURCE_DIR}/include" "${openxr_sdk_SOURCE_DIR}/src/common"
)

add_library(odvr_json INTERFACE)
add_library(odvr::json ALIAS odvr_json)
target_include_directories(odvr_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}/single_include")
# Settings code parses without exceptions (TH-9); keep implicit conversions off
target_compile_definitions(odvr_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)

# C++/WinRT ships with the Windows SDK
find_path(
  ODVR_CPPWINRT_INCLUDE_DIR winrt/base.h
  HINTS
  "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt"
  "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt"
  "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_SYSTEM_VERSION}/cppwinrt"
  REQUIRED
)
add_library(odvr_cppwinrt INTERFACE)
add_library(odvr::cppwinrt ALIAS odvr_cppwinrt)
target_include_directories(odvr_cppwinrt SYSTEM INTERFACE "${ODVR_CPPWINRT_INCLUDE_DIR}")
# windowsapp: C++/WinRT's error handling (winrt::check_hresult)
target_link_libraries(odvr_cppwinrt INTERFACE windowsapp)

find_program(
  ODVR_FXC fxc
  HINTS
  "$ENV{WindowsSdkVerBinPath}x64"
  "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_SYSTEM_VERSION}/x64"
  REQUIRED
)

# Dear ImGui: the library and the two backends we use
add_library(
  odvr_imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)
add_library(odvr::imgui ALIAS odvr_imgui)
target_include_directories(odvr_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
# No ini file next to the exe: window layout isn't persisted by ImGui
target_compile_definitions(odvr_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
# No debug tools: we use none, and with them every ImGui allocation reports
# to the current context, which a font atlas built on a worker thread would
# do on the UI thread's context (SettingsWindow::Prepare)
target_compile_definitions(odvr_imgui PUBLIC IMGUI_DISABLE_DEBUG_TOOLS)
target_link_libraries(odvr_imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)
set_target_properties(odvr_imgui PROPERTIES FOLDER "third-party")
