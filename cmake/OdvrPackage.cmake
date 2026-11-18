# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Robert McGough
#
# What goes into a package, and the ZIP it's packed into:
#
#   cmake --build --preset release
#   cpack --preset test-zip        -> build/package/OpenDesktopVR.zip
#
# Layout (inside the OpenDesktopVR/ folder of the zip):
#   OpenDesktopVR-Layer.dll, OpenDesktopVR-Layer.json   the OpenXR layer
#   OpenDesktopVR.exe                                    the host app (tray)
#   OpenDesktopVR-AppHelper.dll                          loaded into captured apps that need it
#   odvr-layer.exe                                       registration and settings
#   BUILD-INFO.txt                                       version, commit, date
#   LICENSE, THIRD_PARTY_NOTICES, licenses/
#   tools/odvr-mirror.exe                                the panel preview (Diagnostics)
#   install.cmd, uninstall.cmd, command-prompt.cmd,      (component "testtools")
#   tools/self-test.cmd and test tools
#
# Each install() call names a component. The "product" component is what
# OpenDesktopVR-Setup.exe carries (tools/CMakeLists.txt, ADR-043); the test
# zip has both.

install(TARGETS ${ODVR_LAYER_FILE_STEM} RUNTIME DESTINATION . COMPONENT product)
install(
  FILES "$<TARGET_FILE_DIR:${ODVR_LAYER_FILE_STEM}>/${ODVR_LAYER_FILE_STEM}.json"
  DESTINATION .
  COMPONENT product
)
install(TARGETS odvr-layer OpenDesktopVR RUNTIME DESTINATION . COMPONENT product)
# Loaded by the host into captured apps that need it (doc 06), from next to its exe
install(TARGETS ${ODVR_APP_HELPER_FILE_STEM} RUNTIME DESTINATION . COMPONENT product)
# The test zip's double-click install, uninstall and a command prompt opened
# in the install folder (the setup program replaces them, M7a)
install(
  FILES
  "${CMAKE_BINARY_DIR}/tools/install.cmd"
  "${CMAKE_BINARY_DIR}/tools/uninstall.cmd"
  "${CMAKE_BINARY_DIR}/tools/command-prompt.cmd"
  DESTINATION .
  COMPONENT testtools
)
install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION . COMPONENT product)
install(DIRECTORY licenses/ DESTINATION licenses COMPONENT product)
install(TARGETS odvr-mirror RUNTIME DESTINATION tools COMPONENT product)
install(TARGETS openxr-simulator click-target RUNTIME DESTINATION tools COMPONENT testtools)
install(FILES "${CMAKE_BINARY_DIR}/tools/self-test.cmd" DESTINATION tools COMPONENT testtools)

# BUILD-INFO.txt is written when the package is made, so the commit is the
# one actually packaged
install(
  CODE "
    set(ODVR_SOURCE_DIR \"${PROJECT_SOURCE_DIR}\")
    set(ODVR_VERSION \"${ODVR_VERSION}\")
    set(ODVR_LAYER_NAME \"${ODVR_LAYER_NAME}\")
    set(ODVR_REPOSITORY \"${ODVR_REPOSITORY}\")
    set(ODVR_CONFIG \"\${CMAKE_INSTALL_CONFIG_NAME}\")
    set(ODVR_OUTPUT \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/BUILD-INFO.txt\")
    include(\"${PROJECT_SOURCE_DIR}/cmake/OdvrBuildInfo.cmake\")
  "
  COMPONENT product
)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME OpenDesktopVR)
set(CPACK_PACKAGE_VENDOR "Robert McGough")
set(CPACK_PACKAGE_VERSION ${ODVR_VERSION})
set(CPACK_PACKAGE_FILE_NAME OpenDesktopVR)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
set(CPACK_VERBATIM_VARIABLES ON)
include(CPack)
