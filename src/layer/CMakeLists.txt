# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# The OpenXR API layer (doc 03): composition only; thin, bounded, never
# blocks. Pure logic lives in odvr_layerlogic so tests can link it; the DLL
# holds the OpenXR and D3D11 plumbing.

odvr_module(
  layerlogic
  SOURCES
  ExtensionPolicy.cpp
  Composition.cpp
  TestPattern.cpp
  LayerConfig.cpp
  DEPS
  odvr::core
  odvr::settings
  odvr::openxr_headers
)

set(ODVR_LAYER_TARGET ${ODVR_LAYER_FILE_STEM})
add_library(
  ${ODVR_LAYER_TARGET} SHARED
  Hooks.cpp
  Session.cpp
  PanelSwapchain.cpp
  TestPanel.cpp
)
target_link_libraries(
  ${ODVR_LAYER_TARGET}
  PRIVATE odvr::layerlogic odvr::xrutil odvr::gfx odvr::core odvr::cppwinrt d3d11
)
target_compile_options(${ODVR_LAYER_TARGET} PRIVATE ${ODVR_NEW_CODE_WARNINGS})
set_target_properties(${ODVR_LAYER_TARGET} PROPERTIES FOLDER "layer")

# The loader manifest, next to the DLL in every configuration. The disable
# environment variable is required for implicit layers. Names come from
# cmake/OdvrProduct.cmake.
set(ODVR_LAYER_DLL "${ODVR_LAYER_TARGET}.dll")
configure_file(manifest.json.in "${CMAKE_CURRENT_BINARY_DIR}/manifest.json.configured" @ONLY)
file(
  GENERATE
  OUTPUT "$<TARGET_FILE_DIR:${ODVR_LAYER_TARGET}>/${ODVR_LAYER_TARGET}.json"
  INPUT "${CMAKE_CURRENT_BINARY_DIR}/manifest.json.configured"
)
