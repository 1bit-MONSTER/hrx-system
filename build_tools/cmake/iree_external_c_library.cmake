# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(ExternalProject)

# Builds one static C archive whose upstream requires a top-level project.
# The parent owns compiler, sanitizer, ABI, and cross-compilation settings;
# the adapter supplies only upstream feature options and the archive identity.
# Source population is separate so disabled dependencies never reach this path.
function(iree_add_external_c_library name)
  cmake_parse_arguments(PARSE_ARGV 1 _RULE ""
    "SOURCE_DIR;BINARY_DIR;TARGET;ARCHIVE_NAME" "INCLUDE_DIRS;CMAKE_ARGS")
  if(_RULE_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown external C library arguments: ${_RULE_UNPARSED_ARGUMENTS}")
  endif()

  set(_toolchain_args)
  foreach(_variable
      CMAKE_C_COMPILER CMAKE_C_COMPILER_ARG1 CMAKE_C_COMPILER_LAUNCHER
      CMAKE_C_COMPILER_TARGET CMAKE_ASM_COMPILER CMAKE_ASM_NASM_COMPILER
      CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN CMAKE_AR CMAKE_RANLIB
      CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES CMAKE_TOOLCHAIN_FILE
      CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR
      CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK
      CMAKE_FIND_ROOT_PATH CMAKE_FIND_ROOT_PATH_MODE_PROGRAM
      CMAKE_FIND_ROOT_PATH_MODE_LIBRARY CMAKE_FIND_ROOT_PATH_MODE_INCLUDE
      CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
      CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT
      CMAKE_MSVC_RUNTIME_LIBRARY CMAKE_POSITION_INDEPENDENT_CODE
      CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
      CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
      CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_DEBUG
      CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
      CMAKE_EXE_LINKER_FLAGS_MINSIZEREL)
    if(DEFINED ${_variable})
      # ExternalProject restores list values without splitting a single -D
      # argument (notably universal macOS architectures and compiler launchers).
      string(REPLACE ";" "|" _value "${${_variable}}")
      list(APPEND _toolchain_args "-D${_variable}:STRING=${_value}")
    endif()
  endforeach()

  set(_archive_dir "${_RULE_BINARY_DIR}/lib")
  set(_archive "${_archive_dir}/${_RULE_ARCHIVE_NAME}")
  ExternalProject_Add(${name}_project
    SOURCE_DIR "${_RULE_SOURCE_DIR}"
    BINARY_DIR "${_RULE_BINARY_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    LIST_SEPARATOR "|"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG>
      --target "${_RULE_TARGET}"
    # Populated source revisions are owned by the parent, not ExternalProject's
    # download stamp. Let the child build graph observe source changes.
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${_archive}"
    CMAKE_ARGS
      ${_toolchain_args}
      "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${_archive_dir}"
      "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG=${_archive_dir}"
      "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE=${_archive_dir}"
      "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO=${_archive_dir}"
      "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL=${_archive_dir}"
      ${_RULE_CMAKE_ARGS}
  )
  add_library(${name} STATIC IMPORTED GLOBAL)
  set_target_properties(${name} PROPERTIES
    IMPORTED_LOCATION "${_archive}"
    INTERFACE_INCLUDE_DIRECTORIES "${_RULE_INCLUDE_DIRS}"
  )
  add_dependencies(${name} ${name}_project)
endfunction()
