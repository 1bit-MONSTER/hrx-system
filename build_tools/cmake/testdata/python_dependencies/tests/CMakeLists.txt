# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(_ROOT_MODE explicit default)
  set(_PACKAGE_DIRS)
  if(_ROOT_MODE STREQUAL "explicit")
    set(_PACKAGE_DIRS "${PROJECT_SOURCE_DIR}")
  endif()
  iree_py_test(
    NAME "${_ROOT_MODE}"
    SRCS imports_test.py
    ARGS "${IREE_TEST_REVERSE_DEPS}"
    PACKAGE_DIRS ${_PACKAGE_DIRS}
    IMPORTS .
    DEPS fixture::generator
  )
endforeach()
