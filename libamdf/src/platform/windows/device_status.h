// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_PLATFORM_WINDOWS_DEVICE_STATUS_H_
#define AMDF_SRC_PLATFORM_WINDOWS_DEVICE_STATUS_H_

#include "libamdf/src/atomics.h"
#include "libamdf/src/platform/windows/kmt_api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Observed terminal execution failure shared by a KMT device's queues.
// A rejected operation is not evidence that the device has failed. Only an
// explicit removal/hang/fault result, monitored-fence reset indication, or
// terminal execution-state query may publish this latch. Publication does not
// prove retirement or cancel work.
typedef struct amdf_kmt_device_status_t {
  // First confirmed terminal failure; zero until one has been observed.
  amdf_atomic_uint64_t terminal_status;
} amdf_kmt_device_status_t;

// Initializes an unpublished device's failure latch.
void amdf_kmt_device_status_initialize(amdf_kmt_device_status_t* device_status);

// Returns the observed terminal failure without a system call or polling.
amdf_status_t amdf_kmt_device_status_query(
    const amdf_kmt_device_status_t* device_status);

// Records an explicit monitored-fence reset indication without a native query.
// The first terminal failure remains authoritative. Reset notification alone
// does not establish native quiescence or authorize resource release.
void amdf_kmt_device_status_record_reset(
    amdf_kmt_device_status_t* device_status);

// Samples an owned monitored fence whose normal values exclude UINT64_MAX.
// Windows may signal that reserved value on TDR. It is failure, not completion:
// latch device loss and return no new retirement evidence. Normal observation
// is one acquire read and comparison, with no status mutation or native call.
static inline uint64_t amdf_kmt_device_status_query_fence_progress(
    amdf_kmt_device_status_t* device_status,
    const volatile uint64_t* fence_pointer) {
  const uint64_t progress = *fence_pointer;
  MemoryBarrier();
  if (progress == UINT64_MAX) {
    amdf_kmt_device_status_record_reset(device_status);
    return 0;
  }
  return progress;
}

// Classifies a failed operation and preserves its error unless a terminal
// device failure is confirmed. An ambiguous NTSTATUS triggers one execution
// state query on this error path, never on successful submission or progress
// sampling. A failed diagnostic query is returned without marking the device
// lost. A successful ACTIVE query leaves the original operation error intact.
amdf_status_t amdf_kmt_device_status_observe_error(
    amdf_kmt_device_status_t* device_status, const amdf_kmt_api_t* api,
    D3DKMT_HANDLE device, amdf_status_t operation_status);

// Completes a cold mapping/residency operation before publishing its result.
// A nonzero paging point requires both fence retirement and one native device
// execution-state query: even an ordinary retired value may accompany device
// failure. Reset sentinels and confirmed execution failures publish the shared
// device latch. A zero point means the native operation completed synchronously
// and needs no additional wait/query, including during terminal cleanup.
// This may block and issues one diagnostic syscall after deferred completion;
// normal command submission and progress sampling do not use this path.
amdf_status_t amdf_kmt_device_status_wait_for_paging(
    amdf_kmt_device_status_t* device_status, const amdf_kmt_api_t* api,
    D3DKMT_HANDLE device, D3DKMT_HANDLE paging_sync_object,
    const volatile uint64_t* current_paging_fence, uint64_t target_value);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_PLATFORM_WINDOWS_DEVICE_STATUS_H_
