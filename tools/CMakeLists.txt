# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Development tools (doc 02 "tools" module).

# A window that logs every click it receives: the input target for tests
odvr_executable(
  click-target
  PORTED
  SOURCES click-target/click-target.cpp
  DEPS user32 gdi32
)

# Fake OpenXR runtime + game that loads a real API layer DLL
odvr_executable(
  openxr-simulator
  PORTED
  SOURCES openxr-simulator/openxr-simulator.cpp
  DEPS odvr::interaction odvr::openxr_headers odvr::cppwinrt d3d11 windowscodecs
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
  DEPS odvr::hostlink odvr::render odvr::core windowscodecs shell32
)
target_include_directories(odvr-mirror PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
