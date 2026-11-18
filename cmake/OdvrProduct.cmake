# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Product identity, defined once. C++ reads these through Product.hpp; the
# layer manifest is generated from them.

# ADR-019: never changes once anyone has installed a build
set(ODVR_LAYER_NAME "XR_APILAYER_OPENDESKTOPVR_overlay")
set(ODVR_LAYER_DISABLE_ENV "DISABLE_${ODVR_LAYER_NAME}")
# The layer DLL and its manifest: <stem>.dll, <stem>.json
set(ODVR_LAYER_FILE_STEM "OpenDesktopVR-Layer")
# The helper loaded into captured apps (doc 06): <stem>.dll
set(ODVR_APP_HELPER_FILE_STEM "OpenDesktopVR-AppHelper")

# Where the source and the releases live (ADR-044, ADR-045): the GitHub
# repository, "owner/name". Updates come from its Releases.
set(ODVR_REPOSITORY "LittleGroove/OpenDesktopVR")

# The version (ADR-043): the project's, plus a pre-release suffix for the
# beta channel ("beta.1" makes "0.7.1-beta.1"); empty for a release
set(ODVR_PRERELEASE "" CACHE STRING "Pre-release suffix, for example beta.1; empty for a release")
set(ODVR_VERSION "${PROJECT_VERSION}")
if(ODVR_PRERELEASE)
  set(ODVR_VERSION "${PROJECT_VERSION}-${ODVR_PRERELEASE}")
endif()

add_compile_definitions(
  ODVR_VERSION="${ODVR_VERSION}"
  ODVR_REPOSITORY="${ODVR_REPOSITORY}"
  ODVR_LAYER_NAME="${ODVR_LAYER_NAME}"
  ODVR_LAYER_DISABLE_ENV="${ODVR_LAYER_DISABLE_ENV}"
  ODVR_LAYER_FILE_STEM="${ODVR_LAYER_FILE_STEM}"
  ODVR_APP_HELPER_FILE_STEM="${ODVR_APP_HELPER_FILE_STEM}"
)
