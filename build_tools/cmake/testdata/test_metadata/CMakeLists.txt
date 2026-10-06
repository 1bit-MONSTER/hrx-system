# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.26)
project(iree_cmake_test_metadata_test NONE)

if(NOT IREE_REPO_ROOT)
  message(FATAL_ERROR "IREE_REPO_ROOT is required")
endif()

set(IREE_BINARY_DIR "${CMAKE_BINARY_DIR}")
set(IREE_IDE_FOLDER IREE)
set(IREE_PACKAGE_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_macros.cmake")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_configure_testing.cmake")

set(_EMPTY_FILE "${CMAKE_CURRENT_BINARY_DIR}/empty_file")
iree_make_empty_file("${_EMPTY_FILE}")

function(add_metadata_test TEST_NAME BUILD_TARGET)
  set(_SENTINEL "${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME}.built")
  add_custom_command(
    OUTPUT "${_SENTINEL}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_SENTINEL}"
    VERBATIM
  )
  add_custom_target("${BUILD_TARGET}" DEPENDS "${_SENTINEL}")
  add_test(
    NAME "${TEST_NAME}"
    COMMAND
      "${CMAKE_COMMAND}" -E compare_files
      "${_SENTINEL}"
      "${_EMPTY_FILE}"
  )
  iree_register_test_build_targets(
    "${TEST_NAME}"
    TARGETS "${BUILD_TARGET}"
  )
endfunction()

add_metadata_test(host host_root)

add_metadata_test(benchmark benchmark_root)
set_property(TEST benchmark PROPERTY LABELS benchmark)

add_metadata_test(fixture-setup fixture_setup_root)
set_property(TEST fixture-setup PROPERTY FIXTURES_SETUP shared-fixture)

add_metadata_test(fixture-required fixture_required_root)
set_property(TEST fixture-required PROPERTY FIXTURES_REQUIRED shared-fixture)

add_metadata_test(fixture-cleanup fixture_cleanup_root)
set_property(TEST fixture-cleanup PROPERTY FIXTURES_CLEANUP shared-fixture)

add_test(NAME source-only COMMAND "${CMAKE_COMMAND}" -E true)
iree_register_test_build_targets(source-only)

set(_TOOL_SENTINEL "${CMAKE_CURRENT_BINARY_DIR}/tool.built")
add_custom_command(
  OUTPUT "${_TOOL_SENTINEL}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_TOOL_SENTINEL}"
  VERBATIM
)
add_custom_target(tool DEPENDS "${_TOOL_SENTINEL}")
add_custom_target(tool_backed_root)
add_dependencies(tool_backed_root tool)
add_test(
  NAME tool-backed
  COMMAND
    "${CMAKE_COMMAND}" -E compare_files
    "${_TOOL_SENTINEL}"
    "${_EMPTY_FILE}"
)
iree_register_test_build_targets(
  tool-backed
  TARGETS tool_backed_root
)

set(IREE_TEST_METADATA_EXTENSION_FILE "" CACHE FILEPATH
  "Optional CMake test metadata fixture extension")
if(IREE_TEST_METADATA_EXTENSION_FILE)
  include("${IREE_TEST_METADATA_EXTENSION_FILE}")
endif()

iree_finalize_test_build_targets()
