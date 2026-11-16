# SPDX-License-Identifier: LicenseRef-Proprietary
# Copyright (c) 2026 Robert McGough. All rights reserved.
#
# Unit tests run in CI; render tests need a GPU (or WARP) and are not
# registered with CTest by default.

# Ported from the prototype (expected: 111 / 94 / 157 checks, 0 failed)
odvr_test(hand-interaction-test PORTED SOURCES hand-interaction-test.cpp DEPS odvr::interaction)
odvr_test(hand-typing-test PORTED SOURCES hand-typing-test.cpp DEPS odvr::interaction)
odvr_test(keyboard-layout-test PORTED SOURCES keyboard-layout-test.cpp DEPS odvr::keyboard)
odvr_test(hand-render-test PORTED NO_CTEST SOURCES hand-render-test.cpp DEPS odvr::handrender windowscodecs)

# Core and settings (M0 exit: write storm, listener growth)
odvr_test(core-events-test SOURCES core-events-test.cpp DEPS odvr::core)
odvr_test(core-log-test SOURCES core-log-test.cpp DEPS odvr::core)
odvr_test(core-util-test SOURCES core-util-test.cpp DEPS odvr::core)
odvr_test(settings-test SOURCES settings-test.cpp DEPS odvr::settings)

# Layer logic, OpenXR helpers and registration (M1)
odvr_test(layer-logic-test SOURCES layer-logic-test.cpp DEPS odvr::layerlogic odvr::xrutil)
odvr_test(registration-test SOURCES registration-test.cpp DEPS odvr::registration)

# The real layer DLL in the simulator (fake runtime + fake game). "FAIL" in
# the output fails the test: some simulator modes print checks but exit 0.
set(ODVR_SIM_ARGS --layer-name ${ODVR_LAYER_NAME} $<TARGET_FILE:${ODVR_LAYER_FILE_STEM}>)
set(ODVR_SIM_OUT "${CMAKE_CURRENT_BINARY_DIR}/simulator-output")
function(odvr_simulator_test name)
  add_test(NAME ${name} COMMAND openxr-simulator ${ARGN})
  set_tests_properties(
    ${name} PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL"
    ENVIRONMENT "OPENDESKTOPVR_TEST_PANEL=1"
    TIMEOUT 120
  )
endfunction()
# XR-7: a runtime without hand tracking still gets an instance, without it
odvr_simulator_test(sim-no-hand-tracking --no-hand-tracking ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT})
# XR-7: a layer below that can't answer early; hands are still enabled
odvr_simulator_test(sim-early-lookups-fail --early-lookups-fail ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT} --bench 2)
# Both at once: "unknown", refused, retried without
odvr_simulator_test(sim-early-lookups-no-hands --early-lookups-fail --no-hand-tracking ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT})
# The test panel composed as a world-locked quad; bench numbers in the output
odvr_simulator_test(sim-bench-test-panel ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT} --bench 3)
set_tests_properties(sim-bench-test-panel PROPERTIES PASS_REGULAR_EXPRESSION "Layer GPU time per frame")

# M4: the simulator's hand scenarios against the real host app (a private
# instance with its built-in test panels), started by with-host
odvr_executable(with-host SOURCES with-host.cpp)
set_target_properties(with-host PROPERTIES FOLDER "tests")
add_dependencies(with-host OpenDesktopVR openxr-simulator ${ODVR_LAYER_FILE_STEM})
function(odvr_hands_test name)
  add_test(NAME ${name} COMMAND with-host $<TARGET_FILE:OpenDesktopVR> -- $<TARGET_FILE:openxr-simulator> ${ARGN})
  set_tests_properties(${name} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)
endfunction()
# Hands tracked, shown while pointing and not when flat, drawn in the game's
# exact views where the hand is (XR-10, XR-11, XR-13, XR-14)
odvr_hands_test(sim-hands ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/hands)
# The game in STAGE, offset from LOCAL: hands still where they are (XR-12)
odvr_hands_test(sim-hands-stage --stage-offset ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/hands-stage)
# M4b, end to end: the scripted hand taps, sweeps and scrolls a panel showing
# click-target; exactly one click, none from the sweep, the wheel without a
# click, and the latency from the click to the panel showing it (doc 11)
add_dependencies(with-host click-target)
add_test(
  NAME sim-click
  COMMAND with-host --also $<TARGET_FILE:click-target> ${ODVR_SIM_OUT}/click/click-target.log
    $<TARGET_FILE:OpenDesktopVR> --window "Click Target"
    -- $<TARGET_FILE:openxr-simulator> ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/click ${ODVR_SIM_OUT}/click/click-target.log
)
set_tests_properties(sim-click PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)

# The real layer DLL against a minimal fake runtime (WARP; runs without a GPU)
odvr_executable(layer-dll-test SOURCES layer-dll-test.cpp DEPS odvr::core odvr::openxr_headers odvr::cppwinrt d3d11)
target_include_directories(layer-dll-test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
set_target_properties(layer-dll-test PROPERTIES FOLDER "tests")
add_dependencies(layer-dll-test ${ODVR_LAYER_FILE_STEM})
add_test(NAME layer-dll-test COMMAND layer-dll-test $<TARGET_FILE:${ODVR_LAYER_FILE_STEM}>)

# Shared-memory ABI and panel policies (M2)
odvr_test(shared-abi-test SOURCES shared-abi-test.cpp DEPS odvr::xrshared)
odvr_test(panels-test SOURCES panels-test.cpp DEPS odvr::panels)
# The host's panel model: trims, facing, the settings format, ring growth (M3b)
odvr_test(hostcore-test SOURCES hostcore-test.cpp DEPS odvr::hostcore)
# The host's hand thread (M4a): taps, scrolling, tracking gaps (XR-19)
odvr_test(hand-service-test SOURCES hand-service-test.cpp DEPS odvr::hostcore)
# Keybinds (M3c): when bindings fire, capture, back-off, the input thread
odvr_test(keybinds-test SOURCES keybinds-test.cpp DEPS odvr::keybinds)

# M2 end to end: the real host (child processes) and the real layer DLL
odvr_executable(host-layer-test SOURCES host-layer-test.cpp DEPS odvr::xrshared odvr::core odvr::openxr_headers odvr::cppwinrt d3d11 winmm)
target_include_directories(host-layer-test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
set_target_properties(host-layer-test PROPERTIES FOLDER "tests")
add_dependencies(host-layer-test ${ODVR_LAYER_FILE_STEM} OpenDesktopVR)
add_test(NAME host-layer-test COMMAND host-layer-test $<TARGET_FILE:${ODVR_LAYER_FILE_STEM}> $<TARGET_FILE:OpenDesktopVR>)
set_tests_properties(host-layer-test PROPERTIES TIMEOUT 180)

# The app helper (M3d): against a test app that behaves like a Chromium app,
# one module of which delay-loads user32
add_library(apphelper-test-dll SHARED apphelper-test-dll.cpp)
target_link_libraries(apphelper-test-dll PRIVATE user32 delayimp)
target_link_options(apphelper-test-dll PRIVATE /DELAYLOAD:user32.dll)
set_target_properties(apphelper-test-dll PROPERTIES FOLDER "tests")
odvr_executable(apphelper-test-app WIN32 SOURCES apphelper-test-app.cpp DEPS apphelper-test-dll user32)
target_include_directories(apphelper-test-app PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
set_target_properties(apphelper-test-app PROPERTIES FOLDER "tests")
odvr_test(apphelper-test NO_CTEST SOURCES apphelper-test.cpp DEPS odvr::apphelperhost odvr::importpatch odvr::apphelper_protocol)
add_dependencies(apphelper-test apphelper-test-app)
add_test(NAME apphelper-test COMMAND apphelper-test $<TARGET_FILE:apphelper-test-app>)
# The injector (M4b) against click-target, with the helper's input mode
odvr_test(pointer-inject-test NO_CTEST SOURCES pointer-inject-test.cpp DEPS odvr::pointer odvr::apphelperhost)
add_dependencies(pointer-inject-test click-target)
add_test(NAME pointer-inject-test COMMAND pointer-inject-test $<TARGET_FILE:click-target>)
set_tests_properties(pointer-inject-test PROPERTIES TIMEOUT 120)
set_tests_properties(apphelper-test PROPERTIES TIMEOUT 180)

# The app helper with real browsers (M3d, CT-3): Firefox and Edge, each
# skipped if not installed
odvr_test(occlusion-test SOURCES occlusion-test.cpp DEPS odvr::apphelperhost)
set_tests_properties(occlusion-test PROPERTIES TIMEOUT 240)
# The injection matrix's mouse rows on Firefox and Edge, covered (M4b)
odvr_test(browser-input-test SOURCES browser-input-test.cpp DEPS odvr::apphelperhost odvr::pointer)
set_tests_properties(browser-input-test PROPERTIES TIMEOUT 240)

# Window capture against a real window in another process (M3)
odvr_test(capture-test NO_CTEST SOURCES capture-test.cpp DEPS odvr::capture)
target_include_directories(capture-test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
# A game-like GPU load (test round 7, CP-10)
odvr_shaders(capture-test HLSL support/GpuLoad.hlsl ENTRIES "GpuLoadCS|cs_5_0|GpuLoadCS")
add_dependencies(capture-test click-target)
add_test(NAME capture-test COMMAND capture-test $<TARGET_FILE:click-target>)
set_tests_properties(capture-test PROPERTIES TIMEOUT 120)

# M3b: the desktop settings window, in the real host
odvr_test(settings-window-test NO_CTEST SOURCES settings-window-test.cpp DEPS odvr::gfx odvr::core)
add_dependencies(settings-window-test OpenDesktopVR)
add_test(NAME settings-window-test COMMAND settings-window-test $<TARGET_FILE:OpenDesktopVR>)
set_tests_properties(settings-window-test PROPERTIES TIMEOUT 150)
