// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMDXDNA_NATIVE_LINUX_KMQ_INTERNAL_H_
#define IREE_HAL_DRIVERS_AMDXDNA_NATIVE_LINUX_KMQ_INTERNAL_H_

#include "iree/base/api.h"

// Maps Linux BO-allocation errno values to HAL status codes. Exhaustion of the
// shared device heap is recoverable after idle native resources are reclaimed.
iree_status_code_t iree_hal_amdxdna_native_linux_bo_allocation_status_code(
    int error_number);

// Returns whether a public Linux firmware release can implement
// ERT_START_NPU_PREEMPT_ELF. Support is monotonic from the oldest upstream
// release known to carry protocol 6.12; unknown and older versions fail closed
// because probing the opcode can leave old KMQ queues permanently incomplete.
bool iree_hal_amdxdna_native_linux_firmware_supports_full_elf(bool has_version,
                                                              uint32_t major,
                                                              uint32_t minor,
                                                              uint32_t patch,
                                                              uint32_t build);

#endif  // IREE_HAL_DRIVERS_AMDXDNA_NATIVE_LINUX_KMQ_INTERNAL_H_
