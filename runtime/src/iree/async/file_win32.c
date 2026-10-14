// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// FileAlignmentInfo and FileStorageInfo are Windows 8 information classes.
// GetFileInformationByHandleEx reports unsupported queries at runtime.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif  // _WIN32_WINNT

#include <windows.h>

#include "iree/async/file_platform.h"

iree_status_t iree_async_file_query_direct_requirements(
    iree_async_primitive_t primitive,
    iree_async_file_requirements_t* out_requirements) {
  HANDLE handle = (HANDLE)primitive.value.win32_handle;
  FILE_ALIGNMENT_INFO alignment = {0};
  FILE_STORAGE_INFO storage = {0};
  if (!GetFileInformationByHandleEx(handle, FileAlignmentInfo, &alignment,
                                    sizeof(alignment)) ||
      !GetFileInformationByHandleEx(handle, FileStorageInfo, &storage,
                                    sizeof(storage))) {
    DWORD error = GetLastError();
    if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER ||
        error == ERROR_INVALID_FUNCTION) {
      return iree_make_status(
          IREE_STATUS_UNAVAILABLE,
          "filesystem does not report unbuffered I/O alignment requirements");
    }
    return iree_make_status(iree_status_code_from_win32_error(error),
                            "file I/O requirements query failed (error %lu)",
                            (unsigned long)error);
  }
  // AlignmentRequirement is the device's FILE_*_ALIGNMENT mask. Unbuffered
  // file buffers must also be physical-sector aligned even when that device
  // mask is less restrictive. Logical sectors constrain offsets/lengths;
  // physical performance geometry is not an additional request-size limit.
  uint64_t memory_alignment = (uint64_t)alignment.AlignmentRequirement + 1;
  uint32_t physical_alignment = storage.PhysicalBytesPerSectorForAtomicity;
  uint32_t transfer_alignment = storage.LogicalBytesPerSector;
  if (memory_alignment > IREE_HOST_SIZE_MAX ||
      !iree_host_size_is_power_of_two((iree_host_size_t)memory_alignment) ||
      !iree_host_size_is_power_of_two(physical_alignment) ||
      !iree_host_size_is_power_of_two(transfer_alignment)) {
    return iree_make_status(
        IREE_STATUS_UNAVAILABLE,
        "filesystem has no usable unbuffered I/O alignment");
  }
  memory_alignment = iree_max(memory_alignment,
                              iree_max(physical_alignment, transfer_alignment));
  *out_requirements = (iree_async_file_requirements_t){
      .read = {(iree_host_size_t)memory_alignment, transfer_alignment,
               transfer_alignment},
      .write = {(iree_host_size_t)memory_alignment, transfer_alignment,
                transfer_alignment},
  };
  return iree_ok_status();
}
