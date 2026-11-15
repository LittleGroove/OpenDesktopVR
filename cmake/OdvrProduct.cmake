# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Product identity, defined once. C++ reads these through Product.hpp; the
# layer manifest is generated from them.

# ADR-019: never changes once anyone has installed a build
set(ODVR_LAYER_NAME "XR_APILAYER_OPENDESKTOPVR_overlay")
set(ODVR_LAYER_DISABLE_ENV "DISABLE_${ODVR_LAYER_NAME}")
# The layer DLL and its manifest: <stem>.dll, <stem>.json
set(ODVR_LAYER_FILE_STEM "OpenDesktopVR-Layer")

add_compile_definitions(
  ODVR_VERSION="${PROJECT_VERSION}"
  ODVR_LAYER_NAME="${ODVR_LAYER_NAME}"
  ODVR_LAYER_DISABLE_ENV="${ODVR_LAYER_DISABLE_ENV}"
  ODVR_LAYER_FILE_STEM="${ODVR_LAYER_FILE_STEM}"
)
