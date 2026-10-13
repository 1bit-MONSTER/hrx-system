// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/platform/host_memory.h"

#include <sys/mman.h>

#include "libamdf/src/platform/linux/file.h"
#include "libamdf/src/platform/linux/host_cache.h"

amdf_status_t amdf_platform_host_backing_create(
    uint64_t byte_length, amdf_platform_host_backing_t* out_backing) {
  int descriptor = memfd_create("libamdf", MFD_CLOEXEC);
  if (descriptor < 0) {
    return amdf_linux_error(errno);
  }
  amdf_status_t status = AMDF_STATUS_OK;
  void* pointer = MAP_FAILED;
  if (ftruncate(descriptor, (off_t)byte_length) != 0) {
    status = amdf_linux_error(errno);
  } else {
    pointer = mmap(NULL, (size_t)byte_length, PROT_READ | PROT_WRITE,
                   MAP_SHARED, descriptor, 0);
    if (pointer == MAP_FAILED) {
      status = amdf_linux_error(errno);
    }
  }
  if (amdf_status_is_ok(status)) {
    *out_backing = (amdf_platform_host_backing_t){
        .handle = (uintptr_t)descriptor,
        .pointer = pointer,
    };
  } else {
    const amdf_status_t cleanup = amdf_linux_file_close(&descriptor);
    if (!amdf_status_is_ok(cleanup)) {
      status = cleanup;
    }
  }
  return status;
}

amdf_status_t amdf_platform_host_backing_destroy(
    amdf_platform_host_backing_t* backing, uint64_t byte_length) {
  if (munmap(backing->pointer, (size_t)byte_length) != 0) {
    return amdf_linux_error(errno);
  }
  int descriptor = (int)backing->handle;
  return amdf_linux_file_close(&descriptor);
}

amdf_status_t amdf_platform_host_memory_allocate(uint64_t byte_length,
                                                 void** out_pointer) {
  void* pointer = mmap(NULL, (size_t)byte_length, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (pointer == MAP_FAILED) {
    return amdf_linux_error(errno);
  }
  *out_pointer = pointer;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_platform_host_memory_free(void* pointer,
                                             uint64_t byte_length) {
  return munmap(pointer, (size_t)byte_length) == 0 ? AMDF_STATUS_OK
                                                   : amdf_linux_error(errno);
}

amdf_status_t amdf_platform_host_memory_query_cache_line_size(
    uint32_t* out_line_size) {
  return amdf_linux_host_cache_query_line_size(out_line_size);
}

amdf_status_t amdf_platform_host_memory_cache_control(void* pointer,
                                                      uint64_t byte_length,
                                                      uint32_t line_size) {
  amdf_linux_host_cache_transfer(pointer, byte_length, line_size);
  return AMDF_STATUS_OK;
}
