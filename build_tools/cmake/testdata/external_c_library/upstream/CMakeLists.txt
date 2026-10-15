# Copyright 2026 The IREE Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.26)
project(external_c_library_upstream C)
if(NOT CMAKE_C_COMPILER STREQUAL FIXTURE_EXPECTED_COMPILER)
  message(FATAL_ERROR "Nested build changed the selected C compiler")
endif()
list(LENGTH CMAKE_FIND_ROOT_PATH _root_count)
if(NOT _root_count EQUAL 2)
  message(FATAL_ERROR "Nested build did not preserve toolchain list values")
endif()
add_library(value STATIC value.c)
