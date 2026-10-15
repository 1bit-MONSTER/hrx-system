# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(iree_third_party_helpers)

iree_add_alias_interface(iree::third_party::libpng)

function(_iree_configure_libpng_facade target_name)
  get_target_property(_facade_target
    iree::third_party::libpng ALIASED_TARGET)
  target_link_libraries(${_facade_target} INTERFACE ${target_name})
  set_property(TARGET ${_facade_target} PROPERTY IREE_LIBPNG_CONFIGURED TRUE)
endfunction()

function(iree_configure_libpng)
  get_target_property(_facade_target
    iree::third_party::libpng ALIASED_TARGET)
  get_target_property(_already_configured
    ${_facade_target} IREE_LIBPNG_CONFIGURED)
  if(_already_configured)
    return()
  endif()

  iree_dependency_package_discovery_allowed(_package_discovery_allowed)
  if(_package_discovery_allowed)
    find_package(PNG QUIET)
    if(TARGET PNG::PNG)
      _iree_configure_libpng_facade(PNG::PNG)
      return()
    endif()
  endif()

  iree_dependency_require_pinned_source_allowed("libpng")
  iree_configure_zlib()
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(PNG_SHARED OFF)
  set(PNG_STATIC ON)
  set(PNG_FRAMEWORK OFF)
  set(PNG_TESTS OFF)
  set(PNG_TOOLS OFF)
  set(SKIP_INSTALL_ALL ON)
  list(PREPEND CMAKE_MODULE_PATH
    "${IREE_ROOT_DIR}/build_tools/third_party/zlib")
  iree_declare_locked_fetch_content(libpng)
  FetchContent_MakeAvailable(libpng)
  if(NOT TARGET png_static)
    message(FATAL_ERROR "libpng did not provide its static library target")
  endif()
  _iree_configure_libpng_facade(png_static)
endfunction()
