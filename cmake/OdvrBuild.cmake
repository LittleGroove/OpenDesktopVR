# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Shared build settings and the helper functions every module uses, so each
# module's CMakeLists.txt only lists its sources and dependencies.
#
#   odvr_module(<name> SOURCES ... [DEPS ...] [PRIVATE_DEPS ...] [PORTED])
#       Static library odvr_<name> (alias odvr::<name>), public headers in
#       <module dir>/include. PORTED marks code ported from the prototype,
#       which is built at its original warning level.
#   odvr_shaders(<target> HLSL <file> ENTRIES "<header>|<profile>|<entry>" ...)
#       Compiles HLSL entry points with fxc into headers on <target>'s
#       private include path.
#   odvr_test(<name> SOURCES ... [DEPS ...] [NO_CTEST])
#       Test executable; registered with CTest unless NO_CTEST.
#   odvr_executable(<name> SOURCES ... [DEPS ...] [WIN32])
#       Tool or app executable.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# One output folder, so tools find each other (the simulator loads the layer)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_compile_definitions(
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
)
add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /MP)

set(ODVR_NEW_CODE_WARNINGS /W4 /WX)
set(ODVR_PORTED_CODE_WARNINGS /W3 /WX)

function(odvr_module name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "PORTED" "" "SOURCES;DEPS;PRIVATE_DEPS")
  set(target "odvr_${name}")
  add_library(${target} STATIC ${ARG_SOURCES})
  add_library(odvr::${name} ALIAS ${target})
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(${target} PUBLIC ${ARG_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
  if(ARG_PORTED)
    target_compile_options(${target} PRIVATE ${ODVR_PORTED_CODE_WARNINGS})
  else()
    target_compile_options(${target} PRIVATE ${ODVR_NEW_CODE_WARNINGS})
  endif()
  set_target_properties(${target} PROPERTIES FOLDER "modules")
endfunction()

function(odvr_shaders target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "HLSL" "ENTRIES")
  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
  get_filename_component(hlsl "${ARG_HLSL}" ABSOLUTE)
  set(headers)
  foreach(entry IN LISTS ARG_ENTRIES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 header_name)
    list(GET parts 1 profile)
    list(GET parts 2 entry_point)
    set(out "${gen_dir}/${header_name}.h")
    add_custom_command(
      OUTPUT "${out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
      COMMAND "${ODVR_FXC}" /nologo /O3 /T ${profile} /E ${entry_point}
        /Vn g_${header_name} /Fh "${out}" "${hlsl}"
      DEPENDS "${hlsl}"
      COMMENT "fxc ${entry_point} -> ${header_name}.h"
      VERBATIM
    )
    list(APPEND headers "${out}")
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${headers})
  set_target_properties(${target}_shaders PROPERTIES FOLDER "shaders")
  add_dependencies(${target} ${target}_shaders)
  target_include_directories(${target} PRIVATE "${gen_dir}")
endfunction()

function(odvr_executable name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "WIN32;PORTED" "" "SOURCES;DEPS")
  if(ARG_WIN32)
    add_executable(${name} WIN32 ${ARG_SOURCES})
  else()
    add_executable(${name} ${ARG_SOURCES})
  endif()
  target_link_libraries(${name} PRIVATE ${ARG_DEPS})
  if(ARG_PORTED)
    target_compile_options(${name} PRIVATE ${ODVR_PORTED_CODE_WARNINGS})
  else()
    target_compile_options(${name} PRIVATE ${ODVR_NEW_CODE_WARNINGS})
  endif()
endfunction()

function(odvr_test name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_CTEST;PORTED" "" "SOURCES;DEPS")
  set(ported)
  if(ARG_PORTED)
    set(ported PORTED)
  endif()
  odvr_executable(${name} ${ported} SOURCES ${ARG_SOURCES} DEPS ${ARG_DEPS})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
  set_target_properties(${name} PROPERTIES FOLDER "tests")
  if(NOT ARG_NO_CTEST)
    add_test(NAME ${name} COMMAND ${name})
  endif()
endfunction()
