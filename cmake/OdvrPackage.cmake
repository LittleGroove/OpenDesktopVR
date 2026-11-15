# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# What goes into a package, and the ZIP it's packed into:
#
#   cmake --build --preset release
#   cpack --preset test-zip        -> build/package/OpenDesktopVR.zip
#
# Layout (inside the OpenDesktopVR/ folder of the zip):
#   OpenDesktopVR-Layer.dll, OpenDesktopVR-Layer.json   the OpenXR layer
#   OpenDesktopVR.exe                                    the host app (tray)
#   odvr-layer.exe                                       registration and settings
#   install.cmd, uninstall.cmd, command-prompt.cmd       test-build helpers
#   BUILD-INFO.txt                                       version, commit, date
#   LICENSE, THIRD_PARTY_NOTICES, licenses/
#   tools/self-test.cmd and test tools                   (component "testtools")
#
# Each install() call names a component, so a release package can later leave
# the test tools out.

install(TARGETS ${ODVR_LAYER_FILE_STEM} RUNTIME DESTINATION . COMPONENT product)
install(
  FILES "$<TARGET_FILE_DIR:${ODVR_LAYER_FILE_STEM}>/${ODVR_LAYER_FILE_STEM}.json"
  DESTINATION .
  COMPONENT product
)
install(TARGETS odvr-layer OpenDesktopVR RUNTIME DESTINATION . COMPONENT product)
# Until the real installer (M7): double-click install, uninstall and a
# command prompt opened in the install folder
install(
  FILES
  "${CMAKE_BINARY_DIR}/tools/install.cmd"
  "${CMAKE_BINARY_DIR}/tools/uninstall.cmd"
  "${CMAKE_BINARY_DIR}/tools/command-prompt.cmd"
  DESTINATION .
  COMPONENT product
)
install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION . COMPONENT product)
install(DIRECTORY licenses/ DESTINATION licenses COMPONENT product)
install(TARGETS openxr-simulator click-target odvr-mirror RUNTIME DESTINATION tools COMPONENT testtools)
install(FILES "${CMAKE_BINARY_DIR}/tools/self-test.cmd" DESTINATION tools COMPONENT testtools)

# BUILD-INFO.txt is written when the package is made, so the commit is the
# one actually packaged
install(
  CODE "
    set(ODVR_SOURCE_DIR \"${PROJECT_SOURCE_DIR}\")
    set(ODVR_VERSION \"${PROJECT_VERSION}\")
    set(ODVR_LAYER_NAME \"${ODVR_LAYER_NAME}\")
    set(ODVR_CONFIG \"\${CMAKE_INSTALL_CONFIG_NAME}\")
    set(ODVR_OUTPUT \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/BUILD-INFO.txt\")
    include(\"${PROJECT_SOURCE_DIR}/cmake/OdvrBuildInfo.cmake\")
  "
  COMPONENT product
)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME OpenDesktopVR)
set(CPACK_PACKAGE_VENDOR "Robert McGough")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME OpenDesktopVR)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
set(CPACK_VERBATIM_VARIABLES ON)
include(CPack)
