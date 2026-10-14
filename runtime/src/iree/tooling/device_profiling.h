// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_TOOLING_DEVICE_PROFILING_H_
#define IREE_TOOLING_DEVICE_PROFILING_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Owns HAL profiling and periodic flush state requested by command line flags.
typedef struct iree_hal_profiling_from_flags_t iree_hal_profiling_from_flags_t;

// Returns true when command line flags request profiling data that needs
// command buffers to retain profile metadata from creation time.
iree_status_t
iree_hal_profiling_from_flags_requires_retained_command_buffer_metadata(
    bool* out_required);

// Begins any HAL profiling requested by command line flags. No-op if profiling
// is not enabled.
//
// |out_profiling| is set to NULL when no profiling state was created. Otherwise
// the returned state must be passed to iree_hal_end_profiling_from_flags, even
// if the profiled operation fails, so background flush failures and sink
// end-session failures can be observed.
iree_status_t iree_hal_begin_profiling_from_flags(
    iree_hal_device_t* device, iree_allocator_t host_allocator,
    iree_hal_profiling_from_flags_t** out_profiling);

// Begins any HAL profiling requested by command line flags on every device in
// |device_group|. No-op if profiling is not enabled.
//
// The returned state owns one tooling session and shares a single profile sink
// across all devices so a multi-device run produces one profile bundle with one
// producer session per device. The returned state must be passed to
// iree_hal_end_profiling_from_flags, even if the profiled operation fails.
iree_status_t iree_hal_begin_device_group_profiling_from_flags(
    iree_hal_device_group_t* device_group, iree_allocator_t host_allocator,
    iree_hal_profiling_from_flags_t** out_profiling);

// Flushes HAL profiling if |profiling| has an active session. No-op for NULL
// state.
//
// This serializes with the optional periodic flush thread owned by |profiling|.
// Tools using iree_hal_begin_profiling_from_flags should prefer this helper
// over calling iree_hal_device_profiling_flush directly.
iree_status_t iree_hal_flush_profiling_from_flags(
    iree_hal_profiling_from_flags_t* profiling);

// Ends any HAL profiling requested by command line flags. No-op if profiling
// is not enabled.
iree_status_t iree_hal_end_profiling_from_flags(
    iree_hal_profiling_from_flags_t* profiling);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_TOOLING_DEVICE_PROFILING_H_
