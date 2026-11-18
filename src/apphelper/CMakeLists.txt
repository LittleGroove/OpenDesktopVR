# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# The app helper (doc 06; ADR-031): a DLL the host loads into captured apps
# that need it. Built on Windows only, with the static C runtime (like every
# module), so it loads into any app whatever runtime that app ships.
#
#   odvr::apphelper_protocol   the host <-> helper protocol (header only)
#   odvr::importpatch          import-table patching, also used by tests
#   OpenDesktopVR-AppHelper    the DLL

add_library(odvr_apphelper_protocol INTERFACE)
add_library(odvr::apphelper_protocol ALIAS odvr_apphelper_protocol)
target_include_directories(odvr_apphelper_protocol INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

odvr_module(
  importpatch
  SOURCES
  ImportPatch.cpp
)

set(ODVR_APP_HELPER_TARGET ${ODVR_APP_HELPER_FILE_STEM})
add_library(${ODVR_APP_HELPER_TARGET} SHARED AppHelperMain.cpp InputFakes.cpp)
target_link_libraries(${ODVR_APP_HELPER_TARGET} PRIVATE odvr::apphelper_protocol odvr::importpatch user32)
# The keyboard commands' numbers (header only; none of odvr::inject's code)
target_include_directories(${ODVR_APP_HELPER_TARGET} PRIVATE "${PROJECT_SOURCE_DIR}/src/inject/include")
target_compile_options(${ODVR_APP_HELPER_TARGET} PRIVATE ${ODVR_NEW_CODE_WARNINGS})
set_target_properties(${ODVR_APP_HELPER_TARGET} PROPERTIES FOLDER "apphelper")
