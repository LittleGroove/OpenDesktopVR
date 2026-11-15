# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Open Desktop VR: OpenXR layer, host app and tools.
#
#   cmake --preset default
#   cmake --build --preset release
#   ctest --preset release
#
# Offline builds: point FetchContent at local copies of the pinned releases
# (see cmake/OdvrThirdParty.cmake), for example
#   -DFETCHCONTENT_SOURCE_DIR_DIRECTXTK=<dir>
cmake_minimum_required(VERSION 3.25)
project(OpenDesktopVR VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "Open Desktop VR targets Windows with MSVC")
endif()
# ADR-002: 64-bit only
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Open Desktop VR is x64 only (ADR-002)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OdvrBuild)
include(OdvrProduct)
include(OdvrThirdParty)

enable_testing()

add_subdirectory(src)
add_subdirectory(tools)
add_subdirectory(tests)
