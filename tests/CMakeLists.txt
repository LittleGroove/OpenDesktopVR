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

# The real layer DLL against a minimal fake runtime (WARP; runs without a GPU)
odvr_executable(layer-dll-test SOURCES layer-dll-test.cpp DEPS odvr::core odvr::openxr_headers odvr::cppwinrt d3d11)
target_include_directories(layer-dll-test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
set_target_properties(layer-dll-test PROPERTIES FOLDER "tests")
add_dependencies(layer-dll-test ${ODVR_LAYER_FILE_STEM})
add_test(NAME layer-dll-test COMMAND layer-dll-test $<TARGET_FILE:${ODVR_LAYER_FILE_STEM}>)

# Shared-memory ABI and panel policies (M2)
odvr_test(shared-abi-test SOURCES shared-abi-test.cpp DEPS odvr::xrshared)
odvr_test(panels-test SOURCES panels-test.cpp DEPS odvr::panels)
