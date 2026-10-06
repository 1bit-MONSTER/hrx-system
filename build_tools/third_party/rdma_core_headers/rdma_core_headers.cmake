# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(iree_third_party_helpers)

function(iree_configure_rdma_core_headers)
  if(TARGET iree::third_party::rdma_core_headers)
    return()
  endif()

  set(_rdma_core_include_dir "")
  iree_dependency_package_discovery_allowed(_package_discovery_allowed)
  if(_package_discovery_allowed)
    find_path(RDMA_CORE_INCLUDE_DIR NAMES infiniband/verbs.h)
    if(RDMA_CORE_INCLUDE_DIR AND
       EXISTS "${RDMA_CORE_INCLUDE_DIR}/rdma/rdma_cma.h")
      set(_rdma_core_include_dir "${RDMA_CORE_INCLUDE_DIR}")
    else()
      iree_dependency_mode(_mode)
      if(_mode STREQUAL "package")
        message(FATAL_ERROR
          "RDMA requires infiniband/verbs.h and rdma/rdma_cma.h; provide "
          "rdma-core headers through CMAKE_PREFIX_PATH or use pinned mode.")
      endif()
    endif()
    mark_as_advanced(RDMA_CORE_INCLUDE_DIR)
  endif()

  if(NOT _rdma_core_include_dir)
    iree_dependency_require_pinned_source_allowed("rdma_core_headers")
    iree_populate_locked_fetch_content(rdma_core_headers _source_dir)
    set(_rdma_core_include_dir "${_source_dir}/include")
  endif()
  add_library(iree_rdma_core_headers INTERFACE)
  target_include_directories(iree_rdma_core_headers SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${_rdma_core_include_dir}>")
  iree_add_alias_interface(
    iree::third_party::rdma_core_headers iree_rdma_core_headers)
endfunction()
