# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(iree_third_party_helpers)
include(iree_external_c_library)

iree_add_alias_interface(iree::third_party::libwebp)

function(_iree_configure_libwebp_facade target_name)
  get_target_property(_facade_target
    iree::third_party::libwebp ALIASED_TARGET)
  target_link_libraries(${_facade_target} INTERFACE ${target_name})
  set_property(TARGET ${_facade_target} PROPERTY IREE_LIBWEBP_CONFIGURED TRUE)
endfunction()

function(iree_configure_libwebp)
  get_target_property(_facade_target
    iree::third_party::libwebp ALIASED_TARGET)
  get_target_property(_already_configured
    ${_facade_target} IREE_LIBWEBP_CONFIGURED)
  if(_already_configured)
    return()
  endif()

  iree_dependency_package_discovery_allowed(_package_discovery_allowed)
  if(_package_discovery_allowed)
    find_package(WebP CONFIG QUIET)
    foreach(_target_name WebP::webpdecoder webpdecoder WebP::webp webp)
      if(TARGET ${_target_name})
        _iree_configure_libwebp_facade(${_target_name})
        return()
      endif()
    endforeach()
  endif()

  iree_dependency_require_pinned_source_allowed("libwebp")
  iree_declare_locked_fetch_content(libwebp
    SOURCE_SUBDIR iree-external-project-only)
  FetchContent_MakeAvailable(libwebp)

  set(_binary_dir "${libwebp_BINARY_DIR}/project")
  if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    set(_archive_name "webpdecoder${CMAKE_STATIC_LIBRARY_SUFFIX}")
  else()
    set(_archive_name
      "${CMAKE_STATIC_LIBRARY_PREFIX}webpdecoder${CMAKE_STATIC_LIBRARY_SUFFIX}")
  endif()
  iree_add_external_c_library(iree_third_party_libwebp_source
    SOURCE_DIR "${libwebp_SOURCE_DIR}"
    BINARY_DIR "${_binary_dir}"
    TARGET webpdecoder
    ARCHIVE_NAME "${_archive_name}"
    INCLUDE_DIRS "${libwebp_SOURCE_DIR}/src"
    CMAKE_ARGS
      "-DBUILD_SHARED_LIBS=OFF"
      "-DWEBP_BUILD_ANIM_UTILS=OFF"
      "-DWEBP_BUILD_CWEBP=OFF"
      "-DWEBP_BUILD_DWEBP=OFF"
      "-DWEBP_BUILD_GIF2WEBP=OFF"
      "-DWEBP_BUILD_IMG2WEBP=OFF"
      "-DWEBP_BUILD_VWEBP=OFF"
      "-DWEBP_BUILD_WEBPINFO=OFF"
      "-DWEBP_BUILD_LIBWEBPMUX=OFF"
      "-DWEBP_BUILD_WEBPMUX=OFF"
      "-DWEBP_BUILD_EXTRAS=OFF"
      "-DWEBP_BUILD_FUZZTEST=OFF"
      "-DWEBP_USE_THREAD=ON"
  )
  find_package(Threads REQUIRED)
  target_link_libraries(iree_third_party_libwebp_source INTERFACE Threads::Threads)
  _iree_configure_libwebp_facade(iree_third_party_libwebp_source)
endfunction()
