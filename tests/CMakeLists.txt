# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
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
# Crash reports (M6c, TH-10): child processes crash on purpose
odvr_test(crash-test SOURCES crash-test.cpp DEPS odvr::core)
odvr_test(settings-test SOURCES settings-test.cpp DEPS odvr::settings)

# Layer logic, OpenXR helpers and registration (M1)
odvr_test(layer-logic-test SOURCES layer-logic-test.cpp DEPS odvr::layerlogic odvr::xrutil)
odvr_test(registration-test SOURCES registration-test.cpp DEPS odvr::registration)
# Diagnostics and support (M6c): self-test verdicts, the debug bundle, redaction
odvr_test(diagnostics-test SOURCES diagnostics-test.cpp DEPS odvr::diagnostics)

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
# M4c: the same in STAGE, offset from LOCAL: taps still land where aimed
add_test(
  NAME sim-click-stage
  COMMAND with-host --also $<TARGET_FILE:click-target> ${ODVR_SIM_OUT}/click-stage/click-target.log
    $<TARGET_FILE:OpenDesktopVR> --window "Click Target"
    -- $<TARGET_FILE:openxr-simulator> --stage-offset ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/click-stage
      ${ODVR_SIM_OUT}/click-stage/click-target.log
)
set_tests_properties(sim-click-stage PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)
# M4c: the rest of doc 11's hand scenarios: an angled poke, hold then drag
# 40 mm, scroll 36 mm, tracking gaps of 300 and 100 ms in a drag, reaching
# behind the panel, the hand layer's image wait failing once
add_test(
  NAME sim-gestures
  COMMAND with-host --also $<TARGET_FILE:click-target> ${ODVR_SIM_OUT}/gestures/click-target.log
    $<TARGET_FILE:OpenDesktopVR> --window "Click Target"
    -- $<TARGET_FILE:openxr-simulator> --gestures ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/gestures
      ${ODVR_SIM_OUT}/gestures/click-target.log
)
set_tests_properties(sim-gestures PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)
# M5b: doc 11's typing scenario. The keyboard shows from the start (on a PC
# that lets apps take the foreground, the caret goes with it: ADR-035), the
# hand taps the edit-target-app panel, then pokes "h" and "i"; the EDIT gets
# "hi"
add_dependencies(with-host edit-target-app)
add_test(
  NAME sim-keyboard
  COMMAND with-host --also $<TARGET_FILE:edit-target-app> ${ODVR_SIM_OUT}/keyboard/typed.txt
    $<TARGET_FILE:OpenDesktopVR> --window "edit-target" --show-keyboard
    -- $<TARGET_FILE:openxr-simulator> ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/keyboard --type hi ${ODVR_SIM_OUT}/keyboard/typed.txt
)
set_tests_properties(sim-keyboard PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)
# M5c (test round 13, ADR-037): recentering. The head moves; the runtime
# recenters with LOCAL's new origin above the head, as VDXR did; then the
# host's Recenter keybind. Panels must end up placed from the head each time
add_test(
  NAME sim-recenter
  COMMAND with-host $<TARGET_FILE:OpenDesktopVR> --test-recenter-after 15
    -- $<TARGET_FILE:openxr-simulator> --recenter ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/recenter
)
set_tests_properties(sim-recenter PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)
# M6d (ADR-041): the quick menu, tapped end to end: a panel's switch hides
# it and shows it again, the close button leaves its button, the button
# opens it (DU-6, DU-7, CT-10)
add_test(
  NAME sim-quick-menu
  COMMAND with-host $<TARGET_FILE:OpenDesktopVR> --show-quick-menu
    -- $<TARGET_FILE:openxr-simulator> --quick-menu ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/quick-menu
)
set_tests_properties(sim-quick-menu PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)
# M4c: two panels overlapping in depth, given nearest first: back to front in
# every frame
add_test(
  NAME sim-depth-order
  COMMAND with-host $<TARGET_FILE:OpenDesktopVR> --overlapping-panels
    -- $<TARGET_FILE:openxr-simulator> --depth-order ${ODVR_SIM_ARGS} ${ODVR_SIM_OUT}/depth-order
)
set_tests_properties(sim-depth-order PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" PASS_REGULAR_EXPRESSION "PASSED" TIMEOUT 200)

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
# Profiles (M6a): files, unreadable ones, export and import, games, migration
odvr_test(profiles-test SOURCES profiles-test.cpp DEPS odvr::hostcore)
# The installer's parts (M7a, ADR-043): versions, the payload, files, the
# shortcut, Apps and Features, Store apps' access, in a private folder and key
odvr_test(install-test SOURCES install-test.cpp DEPS odvr::install odvr::core ole32)
# Two real DLLs from the output folder, one loaded while the other replaces it
add_dependencies(install-test ${ODVR_LAYER_FILE_STEM} ${ODVR_APP_HELPER_FILE_STEM})
# Updates from GitHub Releases (ADR-045): the release list, the choice, the
# HTTPS client against a fake server on 127.0.0.1
odvr_test(update-test SOURCES update-test.cpp DEPS odvr::update odvr::core ws2_32)
# Updates end to end: the installed host, a fake GitHub, the real setup
# program as the release; the beta, a bad checksum, an install by itself
odvr_test(update-host-test NO_CTEST SOURCES update-host-test.cpp DEPS odvr::install odvr::settings odvr::core ws2_32)
add_dependencies(update-host-test odvr-setup)
add_test(NAME update-host-test COMMAND update-host-test "$<TARGET_FILE_DIR:OpenDesktopVR>/OpenDesktopVR-Setup.exe")
set_tests_properties(update-host-test PROPERTIES TIMEOUT 300)
# The real OpenDesktopVR-Setup.exe, silent, in a private folder and key:
# install, update over a running host and a held layer, uninstall (M7a)
odvr_test(setup-test NO_CTEST SOURCES setup-test.cpp DEPS odvr::install odvr::xrshared odvr::core)
add_dependencies(setup-test odvr-setup)
add_test(NAME setup-test COMMAND setup-test "$<TARGET_FILE_DIR:OpenDesktopVR>/OpenDesktopVR-Setup.exe"
  $<TARGET_FILE:odvr-setup-stub>)
set_tests_properties(setup-test PROPERTIES TIMEOUT 300)
# The real host moving version 1's panels into a profile, in a private folder;
# a fresh install's first start after the installer's layer settings (round 22)
odvr_test(settings-migration-test NO_CTEST SOURCES settings-migration-test.cpp DEPS odvr::settings odvr::core)
add_dependencies(settings-migration-test OpenDesktopVR odvr-layer)
add_test(NAME settings-migration-test COMMAND settings-migration-test $<TARGET_FILE:OpenDesktopVR> $<TARGET_FILE:odvr-layer>)
set_tests_properties(settings-migration-test PROPERTIES TIMEOUT 120)
# The virtual keyboard's logic (M5b): geometry, keys, repeat, when it shows
odvr_test(vkeyboard-test SOURCES vkeyboard-test.cpp DEPS odvr::vkeyboard)
# Draws the keyboard to PNGs for looking at (needs a GPU or WARP)
odvr_test(keyboard-render-test NO_CTEST SOURCES keyboard-render-test.cpp DEPS odvr::vkeyboardui windowscodecs)
# The quick menu (M6d): every page's targets (DU-6), touches (DU-7), where
# commands go, its model (RN-6) and queue (TH-5), settings, its panel
odvr_test(quickmenu-test SOURCES quickmenu-test.cpp DEPS odvr::hostcore)
# Draws the quick menu's pages to PNGs for looking at (needs a GPU or WARP)
odvr_test(quickmenu-render-test NO_CTEST SOURCES quickmenu-render-test.cpp DEPS odvr::hostcore windowscodecs)
# Automation (M6e, UI-9): commands parsed, refused with reasons, packed for
# the host; fuzzed (TH-9); nudges snap
odvr_test(automation-test SOURCES automation-test.cpp DEPS odvr::automation odvr::hostcore)
# The real host and the real program run as commands: done, refused, busy,
# logged and saved
odvr_test(automation-host-test NO_CTEST SOURCES automation-host-test.cpp DEPS odvr::automation odvr::xrshared odvr::settings odvr::core)
add_dependencies(automation-host-test OpenDesktopVR)
add_test(NAME automation-host-test COMMAND automation-host-test $<TARGET_FILE:OpenDesktopVR>)
set_tests_properties(automation-host-test PROPERTIES TIMEOUT 180)
# M6f, doc 11's robustness tests with the real host: unreadable settings and
# profiles, profile switches with the settings window open, the shared
# memory's name held by another program (an elevated game)
odvr_test(robustness-host-test NO_CTEST SOURCES robustness-host-test.cpp DEPS odvr::xrshared odvr::settings odvr::core advapi32)
add_dependencies(robustness-host-test OpenDesktopVR)
add_test(NAME robustness-host-test COMMAND robustness-host-test $<TARGET_FILE:OpenDesktopVR>)
set_tests_properties(robustness-host-test PROPERTIES TIMEOUT 180)

# M2 end to end: the real host (child processes) and the real layer DLL
odvr_executable(host-layer-test SOURCES host-layer-test.cpp DEPS odvr::xrshared odvr::automation odvr::core odvr::openxr_headers odvr::cppwinrt d3d11 winmm)
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
odvr_test(apphelper-test NO_CTEST SOURCES apphelper-test.cpp DEPS odvr::apphelperhost odvr::importpatch odvr::apphelper_protocol odvr::inject)
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
# The injection matrix's keyboard rows on a Win32 EDIT, Firefox and Edge,
# covered (M5a)
odvr_executable(edit-target-app WIN32 SOURCES edit-target-app.cpp DEPS user32)
set_target_properties(edit-target-app PROPERTIES FOLDER "tests")
odvr_test(key-inject-test NO_CTEST SOURCES key-inject-test.cpp DEPS odvr::apphelperhost odvr::pointer odvr::keyboard odvr::vkeyboard odvr::core)
add_dependencies(key-inject-test edit-target-app)
add_test(NAME key-inject-test COMMAND key-inject-test $<TARGET_FILE:edit-target-app>)
set_tests_properties(key-inject-test PROPERTIES TIMEOUT 300)

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

# Tests that need a real PC's desktop: they inject input into real windows,
# need the foreground, open browsers, capture windows, or hold the program
# to its speed budgets. CI (shared machines, software rendering) skips them
# with "ctest -LE desktop"; they run locally, and before every release.
set_tests_properties(
  apphelper-test pointer-inject-test browser-input-test key-inject-test occlusion-test capture-test
  settings-window-test host-layer-test sim-click sim-click-stage sim-gestures sim-keyboard sim-bench-test-panel
  PROPERTIES LABELS desktop
)
