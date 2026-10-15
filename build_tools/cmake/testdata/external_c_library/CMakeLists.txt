# Copyright 2026 The IREE Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.26)
project(iree_external_c_library_fixture C)

include("${IREE_REPO_ROOT}/build_tools/cmake/iree_external_c_library.cmake")
string(APPEND CMAKE_C_FLAGS " -DFIXTURE_VALUE=41")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_CURRENT_BINARY_DIR}/first root;${CMAKE_CURRENT_BINARY_DIR}/second root")
iree_add_external_c_library(fixture
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/upstream"
  TARGET value
  ARCHIVE_NAME "${CMAKE_STATIC_LIBRARY_PREFIX}value${CMAKE_STATIC_LIBRARY_SUFFIX}"
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
  CMAKE_ARGS "-DFIXTURE_EXPECTED_COMPILER=${CMAKE_C_COMPILER}"
)
add_executable(probe main.c)
target_link_libraries(probe PRIVATE fixture)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/program-$<CONFIG>.txt"
  CONTENT "$<TARGET_FILE:probe>\n")
