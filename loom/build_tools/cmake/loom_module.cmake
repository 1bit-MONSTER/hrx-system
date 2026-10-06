# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 WITH LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Loom module linking helpers.

include_guard(GLOBAL)

# Authoring tools execute on the build host, including when their output will
# be embedded in a library compiled for another platform.
function(_loom_host_tool OUTPUT_TOOL NAME)
  if(IREE_HOST_BIN_DIR)
    set(_HOST_SUFFIX "")
    if(CMAKE_HOST_WIN32)
      set(_HOST_SUFFIX ".exe")
    endif()
    file(REAL_PATH "${IREE_HOST_BIN_DIR}" _HOST_BIN_DIR
      BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" EXPAND_TILDE)
    set(_PATH "${_HOST_BIN_DIR}/${NAME}${_HOST_SUFFIX}")
    if(NOT EXISTS "${_PATH}")
      message(FATAL_ERROR
        "Loom generation requires host tool ${_PATH}; "
        "provide ${NAME} in IREE_HOST_BIN_DIR.")
    endif()
    set(${OUTPUT_TOOL} "${_PATH}" PARENT_SCOPE)
  elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
      "Cross-compiling Loom products requires IREE_HOST_BIN_DIR "
      "with a build-host ${NAME} executable.")
  else()
    set(${OUTPUT_TOOL} "$<TARGET_FILE:loom::tools::${NAME}>" PARENT_SCOPE)
  endif()
endfunction()

# Resolve the declared library graph once every package has registered its
# targets. Direct dependencies remain distinct from the transitive audit
# universe, matching the public linker's strict dependency contract.
function(_loom_module_resolve_libraries TARGET_NAME)
  get_property(_RESOLVED TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_LIBRARIES SET)
  if(_RESOLVED)
    return()
  endif()
  get_property(_RESOLVING TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_RESOLVING)
  if(_RESOLVING)
    message(FATAL_ERROR "Cyclic Loom library dependency: ${TARGET_NAME}")
  endif()
  set_property(TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_RESOLVING TRUE)
  get_property(_DIRECT TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_DIRECT_LIBRARIES)
  get_property(_TARGETS TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_LIBRARY_TARGETS)
  set(_TRANSITIVE)
  foreach(_DEPENDENCY IN LISTS _TARGETS)
    _loom_module_resolve_libraries("${_DEPENDENCY}")
    get_property(_CLOSURE TARGET "${_DEPENDENCY}" PROPERTY LOOM_MODULE_LIBRARIES)
    list(APPEND _TRANSITIVE ${_CLOSURE})
  endforeach()
  list(REMOVE_DUPLICATES _TRANSITIVE)
  if(_DIRECT)
    list(REMOVE_ITEM _TRANSITIVE ${_DIRECT})
  endif()
  set_property(TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_TRANSITIVE_LIBRARIES "${_TRANSITIVE}")
  set_property(TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_LIBRARIES "${_DIRECT};${_TRANSITIVE}")
  set_property(TARGET "${TARGET_NAME}" PROPERTY LOOM_MODULE_RESOLVING FALSE)
endfunction()

function(loom_finalize_module_libraries)
  get_property(_TARGETS GLOBAL PROPERTY LOOM_MODULE_TARGETS)
  foreach(_TARGET IN LISTS _TARGETS)
    _loom_module_resolve_libraries("${_TARGET}")
  endforeach()
endfunction()

function(_loom_link_input_paths OUTPUT_PATHS OUTPUT_TARGETS)
  set(_PATHS)
  set(_TARGETS)
  foreach(_INPUT IN LISTS ARGN)
    if("${_INPUT}" MATCHES "::")
      iree_package_target_name(_TARGET "${_INPUT}")
      list(APPEND _PATHS "$<TARGET_PROPERTY:${_TARGET},LOOM_MODULE_FILE>")
      list(APPEND _TARGETS "${_TARGET}")
    elseif(IS_ABSOLUTE "${_INPUT}" OR "${_INPUT}" MATCHES "^\\$<")
      list(APPEND _PATHS "${_INPUT}")
    else()
      list(APPEND _PATHS "${CMAKE_CURRENT_SOURCE_DIR}/${_INPUT}")
    endif()
  endforeach()
  set(${OUTPUT_PATHS} ${_PATHS} PARENT_SCOPE)
  set(${OUTPUT_TARGETS} ${_TARGETS} PARENT_SCOPE)
endfunction()

function(loom_module)
  cmake_parse_arguments(
    _RULE
    "INCLUDE_INPUT_EXPORTS;INCLUDE_INPUT_TESTS;STRIP_CHECK;REQUIRE_RESOLVED_CONFIG;STRICT_DEPS"
    "NAME;MODE;OUTPUT;OUTPUT_FORMAT;INPUT_FORMAT"
    "SRCS;LIBRARIES;ROOTS;CONFIGS;DATA;INPUTOPTS"
    ${ARGN}
  )

  if(NOT _RULE_NAME)
    message(FATAL_ERROR "loom_module requires NAME")
  endif()
  if(NOT _RULE_SRCS AND NOT _RULE_LIBRARIES)
    message(FATAL_ERROR "loom_module requires SRCS or LIBRARIES")
  endif()
  if(NOT _RULE_MODE)
    set(_RULE_MODE "merge")
  endif()
  if(NOT _RULE_MODE STREQUAL "merge" AND NOT _RULE_MODE STREQUAL "link")
    message(FATAL_ERROR
      "loom_module ${_RULE_NAME} has unsupported MODE ${_RULE_MODE}"
    )
  endif()
  if(_RULE_MODE STREQUAL "merge" AND
     (_RULE_ROOTS OR _RULE_INCLUDE_INPUT_EXPORTS OR _RULE_INCLUDE_INPUT_TESTS))
    message(FATAL_ERROR
      "loom_module ${_RULE_NAME} merge mode does not accept roots"
    )
  endif()
  if(_RULE_MODE STREQUAL "link" AND
     NOT _RULE_ROOTS AND NOT _RULE_INCLUDE_INPUT_EXPORTS AND NOT _RULE_INCLUDE_INPUT_TESTS)
    message(FATAL_ERROR
      "loom_module ${_RULE_NAME} link mode requires ROOTS, INCLUDE_INPUT_EXPORTS, or INCLUDE_INPUT_TESTS"
    )
  endif()
  if(NOT _RULE_OUTPUT_FORMAT)
    set(_RULE_OUTPUT_FORMAT "text")
  endif()
  if(NOT _RULE_OUTPUT_FORMAT STREQUAL "text" AND
     NOT _RULE_OUTPUT_FORMAT STREQUAL "bc")
    message(FATAL_ERROR
      "loom_module ${_RULE_NAME} has unsupported OUTPUT_FORMAT ${_RULE_OUTPUT_FORMAT}"
    )
  endif()
  if(NOT _RULE_OUTPUT)
    if(_RULE_OUTPUT_FORMAT STREQUAL "bc")
      set(_RULE_OUTPUT "${_RULE_NAME}.loombc")
    else()
      set(_RULE_OUTPUT "${_RULE_NAME}.loom")
    endif()
  endif()

  _loom_link_input_paths(_SOURCES _SOURCE_TARGETS ${_RULE_SRCS})
  _loom_link_input_paths(_LIBRARIES _LIBRARY_TARGETS ${_RULE_LIBRARIES})
  iree_package_name(_PACKAGE_NAME)
  set(_TARGET "${_PACKAGE_NAME}_${_RULE_NAME}")
  set(_TRANSITIVE_LIBRARIES "$<TARGET_GENEX_EVAL:${_TARGET},$<TARGET_PROPERTY:${_TARGET},LOOM_MODULE_TRANSITIVE_LIBRARIES>>")
  set(_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_RULE_OUTPUT}")
  set(_ARGS
    "--mode=${_RULE_MODE}"
    "--to=${_RULE_OUTPUT_FORMAT}"
    ${_SOURCES}
  )
  foreach(_LIBRARY IN LISTS _LIBRARIES)
    list(APPEND _ARGS "--library=${_LIBRARY}")
  endforeach()
  list(APPEND _ARGS
    "$<$<BOOL:${_TRANSITIVE_LIBRARIES}>:--transitive-library=$<JOIN:${_TRANSITIVE_LIBRARIES},$<SEMICOLON>--transitive-library=>>")
  foreach(_ROOT IN LISTS _RULE_ROOTS)
    list(APPEND _ARGS "--root=${_ROOT}")
  endforeach()
  foreach(_CONFIG IN LISTS _RULE_CONFIGS)
    list(APPEND _ARGS "--config=${_CONFIG}")
  endforeach()
  if(_RULE_INCLUDE_INPUT_EXPORTS)
    list(APPEND _ARGS "--include-input-exports=true")
  endif()
  if(_RULE_INCLUDE_INPUT_TESTS)
    list(APPEND _ARGS "--include-input-tests")
  endif()
  if(_RULE_INPUT_FORMAT)
    list(APPEND _ARGS "--input-format=${_RULE_INPUT_FORMAT}")
  endif()
  foreach(_OPTIONS IN LISTS _RULE_INPUTOPTS)
    list(APPEND _ARGS "--input-options=${_OPTIONS}")
  endforeach()
  if(_RULE_STRIP_CHECK)
    list(APPEND _ARGS "--strip-check=true")
  endif()
  if(_RULE_REQUIRE_RESOLVED_CONFIG)
    list(APPEND _ARGS "--require-resolved-config=true")
  endif()
  if(_RULE_STRICT_DEPS)
    list(APPEND _ARGS "--strict-deps")
  endif()
  list(APPEND _ARGS "--output=${_OUTPUT}")
  _loom_host_tool(_LINK_TOOL loom-link)

  add_custom_command(
    OUTPUT
      "${_OUTPUT}"
    COMMAND
      "${_LINK_TOOL}" "${_ARGS}"
    DEPENDS
      "${_LINK_TOOL}"
      ${_SOURCES}
      "${_LIBRARIES}"
      "${_TRANSITIVE_LIBRARIES}"
      ${_RULE_DATA}
    WORKING_DIRECTORY
      "${PROJECT_SOURCE_DIR}"
    COMMENT
      "Linking Loom module ${_RULE_OUTPUT}"
    VERBATIM
    COMMAND_EXPAND_LISTS
  )
  set_source_files_properties(
    "${_RULE_OUTPUT}"
    "${_OUTPUT}"
    PROPERTIES GENERATED TRUE
  )

  add_custom_target("${_TARGET}" DEPENDS "${_OUTPUT}")
  set_property(TARGET "${_TARGET}" PROPERTY LOOM_MODULE_FILE "${_OUTPUT}")
  set_property(TARGET "${_TARGET}" PROPERTY LOOM_MODULE_DIRECT_LIBRARIES "${_LIBRARIES}")
  set_property(TARGET "${_TARGET}" PROPERTY LOOM_MODULE_LIBRARY_TARGETS "${_LIBRARY_TARGETS}")
  set_property(GLOBAL APPEND PROPERTY LOOM_MODULE_TARGETS "${_TARGET}")
  foreach(_INPUT_TARGET IN LISTS _SOURCE_TARGETS _LIBRARY_TARGETS)
    iree_register_target_dependency(
      TARGET "${_TARGET}"
      DEPENDENCY "${_INPUT_TARGET}"
    )
  endforeach()
  foreach(_INPUT IN LISTS _SOURCES _LIBRARIES _RULE_DATA)
    iree_generated_output_add_consumer("${_INPUT}" "${_TARGET}")
  endforeach()
  iree_register_generated_output_producer("${_TARGET}"
    OUTPUTS "${_OUTPUT}"
  )
endfunction()
