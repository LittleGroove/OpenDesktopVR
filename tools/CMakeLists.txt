# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Development tools (doc 02 "tools" module).

# A window that logs every click it receives: the input target for tests
odvr_executable(
  click-target
  PORTED
  SOURCES click-target/click-target.cpp
  DEPS user32 gdi32 dwmapi
)

# Fake OpenXR runtime + game that loads a real API layer DLL
odvr_executable(
  openxr-simulator
  PORTED
  SOURCES openxr-simulator/openxr-simulator.cpp
  DEPS odvr::interaction odvr::vkeyboard odvr::quickmenu odvr::openxr_headers odvr::cppwinrt d3d11 windowscodecs
)
target_include_directories(openxr-simulator PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")

# Register the layer and edit its settings from a terminal (until the host
# app does it, M2)
odvr_executable(
  odvr-layer
  SOURCES odvr-layer/odvr-layer.cpp
  DEPS odvr::registration odvr::settings odvr::core
)

# The packaged self-test (names from cmake/OdvrProduct.cmake)
configure_file(self-test/self-test.cmd.in "${CMAKE_CURRENT_BINARY_DIR}/self-test.cmd" @ONLY NEWLINE_STYLE CRLF)

# Test-build install helpers, copied with Windows line endings (cmd can
# misread labels in LF-only batch files)
foreach(script install uninstall command-prompt)
  configure_file(package/${script}.cmd "${CMAKE_CURRENT_BINARY_DIR}/${script}.cmd" @ONLY NEWLINE_STYLE CRLF)
endforeach()

# Shows the panel images a layer receives, in a desktop window (doc 02)
odvr_executable(
  odvr-mirror
  WIN32
  SOURCES mirror/odvr-mirror.cpp
  DEPS odvr::hostlink odvr::render odvr::gfx odvr::core shell32
)

# The setup program (M7a, ADR-043), built without its files: odvr-pack adds
# them, from cmake --install's "product" component, to make
# bin/<config>/OpenDesktopVR-Setup.exe
odvr_executable(
  odvr-setup-stub
  WIN32
  SOURCES setup/SetupMain.cpp setup/setup.manifest
  DEPS odvr::install odvr::registration odvr::core comctl32 shell32
)
odvr_executable(
  odvr-pack
  SOURCES setup/odvr-pack.cpp
  DEPS odvr::install odvr::core
)
set(ODVR_SETUP_STAGE "${CMAKE_BINARY_DIR}/setup-stage/$<CONFIG>")
set(ODVR_SETUP_EXE "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/OpenDesktopVR-Setup.exe")
add_custom_command(
  OUTPUT "${ODVR_SETUP_EXE}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${ODVR_SETUP_STAGE}"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config $<CONFIG> --component product
    --prefix "${ODVR_SETUP_STAGE}"
  COMMAND odvr-pack $<TARGET_FILE:odvr-setup-stub> "${ODVR_SETUP_STAGE}" "${ODVR_SETUP_EXE}" --version "${ODVR_VERSION}"
  DEPENDS odvr-setup-stub odvr-pack OpenDesktopVR odvr-layer odvr-mirror ${ODVR_LAYER_FILE_STEM}
    ${ODVR_APP_HELPER_FILE_STEM} "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES"
  COMMENT "Making OpenDesktopVR-Setup.exe"
  VERBATIM
)
add_custom_target(odvr-setup ALL DEPENDS "${ODVR_SETUP_EXE}")
