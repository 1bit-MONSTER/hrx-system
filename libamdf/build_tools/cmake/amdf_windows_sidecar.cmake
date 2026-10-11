# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

function(_amdf_windows_sidecar_compile_options)
  foreach(_TARGET IN LISTS ARGN)
    set_property(TARGET ${_TARGET} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_definitions(${_TARGET}
      PRIVATE
        _DISABLE_STRING_ANNOTATION
        _DISABLE_VECTOR_ANNOTATION
        _ITERATOR_DEBUG_LEVEL=0
    )
  endforeach()
endfunction()

# Builds a private Windows DLL whose complete C++ and CRT ABI stays behind a C
# entry point. Binary-only dependencies use their release static CRT in every
# parent build configuration.
function(amdf_windows_sidecar_library)
  cmake_parse_arguments(
    _RULE
    "TESTONLY"
    "NAME"
    "SRCS;DEPS;LINKOPTS"
    ${ARGN}
  )
  if(_RULE_TESTONLY AND NOT IREE_BUILD_TESTS)
    return()
  endif()
  if(NOT _RULE_NAME)
    message(FATAL_ERROR "amdf_windows_sidecar_library requires NAME.")
  endif()
  if(NOT _RULE_SRCS)
    message(FATAL_ERROR
      "amdf_windows_sidecar_library requires at least one source file.")
  endif()
  set(_TESTONLY)
  if(_RULE_TESTONLY)
    set(_TESTONLY TESTONLY)
  endif()

  iree_cc_library(
    NAME
      "${_RULE_NAME}"
    SRCS
      ${_RULE_SRCS}
    DEPS
      ${_RULE_DEPS}
    LINKOPTS
      ${_RULE_LINKOPTS}
    SHARED
    ${_TESTONLY}
  )

  iree_package_name(_PACKAGE_NAME)
  set(_TARGET_NAME "${_PACKAGE_NAME}_${_RULE_NAME}")
  _amdf_windows_sidecar_compile_options(
    ${_TARGET_NAME}
  )
  set_target_properties(${_TARGET_NAME} PROPERTIES
    OUTPUT_NAME "${_RULE_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:amdf>"
  )
  if(NOT _RULE_TESTONLY)
    install(
      TARGETS ${_TARGET_NAME}
      COMPONENT AMDF
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
  endif()
endfunction()

# Standalone implementation tests share the DLL's private release ABI.
function(amdf_windows_sidecar_test)
  if(NOT IREE_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(_RULE "" "NAME" "" ${ARGN})
  iree_cc_test(${ARGN})
  iree_package_name(_PACKAGE_NAME)
  _amdf_windows_sidecar_compile_options("${_PACKAGE_NAME}_${_RULE_NAME}")
endfunction()
