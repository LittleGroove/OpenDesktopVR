# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Writes BUILD-INFO.txt at install/package time. Expects ODVR_SOURCE_DIR,
# ODVR_VERSION, ODVR_LAYER_NAME, ODVR_REPOSITORY, ODVR_CONFIG and ODVR_OUTPUT.

find_package(Git QUIET)
set(commit "unknown")
set(branch "unknown")
set(state "")
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
    WORKING_DIRECTORY "${ODVR_SOURCE_DIR}"
    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY "${ODVR_SOURCE_DIR}"
    OUTPUT_VARIABLE branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${ODVR_SOURCE_DIR}"
    OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
  )
  if(changes)
    set(state " (with uncommitted changes)")
  endif()
endif()
string(TIMESTAMP built "%Y-%m-%d %H:%M")

file(
  WRITE "${ODVR_OUTPUT}"
  "Open Desktop VR ${ODVR_VERSION}\n"
  "Commit:      ${commit}${state} on ${branch}\n"
  "Built:       ${built} (${ODVR_CONFIG})\n"
  "OpenXR layer: ${ODVR_LAYER_NAME}\n"
  "\n"
  "Free software under the GNU GPL, version 3 (see LICENSE), with no\n"
  "warranty. Source code: https://github.com/${ODVR_REPOSITORY}\n"
)
