// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMD_XDNA_PROFILE_H_
#define IREE_HAL_DRIVERS_AMD_XDNA_PROFILE_H_

#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bounded host-clock capture. Queue events describe public submissions. Host
// execution spans cover native preparation/publication and mapped host work;
// they exclude device execution and asynchronous file I/O latency.
typedef struct iree_hal_amd_xdna_profile_t iree_hal_amd_xdna_profile_t;

// Begins a session and emits its device/queue identity. Supports QUEUE_EVENTS
// and HOST_EXECUTION_EVENTS; LIGHTWEIGHT_STATISTICS selects both. Other
// families require native instrumentation and are rejected. An empty request
// returns NULL. The sink is retained until end. Begin/end require the HAL
// idle-device contract; append operations never allocate or invoke sink
// callbacks.
iree_status_t iree_hal_amd_xdna_profile_begin(
    const iree_hal_device_profiling_options_t* options, uint64_t session_id,
    uint32_t physical_device_ordinal, uint32_t queue_ordinal,
    iree_allocator_t host_allocator, iree_hal_amd_xdna_profile_t** out_profile);

// Writes completed event snapshots without holding the recording mutex.
// Failed writes leave the snapshot available for a subsequent flush.
iree_status_t iree_hal_amd_xdna_profile_flush(
    iree_hal_amd_xdna_profile_t* profile);

// Flushes and ends a session, consuming profile even if sink callbacks fail.
iree_status_t iree_hal_amd_xdna_profile_end(
    iree_hal_amd_xdna_profile_t* profile);

// Assigns submission identity and host timestamp to a captured queue event.
void iree_hal_amd_xdna_profile_initialize_event(
    iree_hal_amd_xdna_profile_t* profile,
    iree_hal_profile_queue_event_t* event);

// Appends a finalized queue event. Full rings account dropped records for
// flush.
void iree_hal_amd_xdna_profile_append_queue(
    iree_hal_amd_xdna_profile_t* profile,
    const iree_hal_profile_queue_event_t* event);

// Samples the host clock only when host execution capture is enabled.
iree_time_t iree_hal_amd_xdna_profile_host_begin(
    iree_hal_amd_xdna_profile_t* profile);

// Records a completed span; a zero start denotes disabled host capture.
void iree_hal_amd_xdna_profile_host_end(
    iree_hal_amd_xdna_profile_t* profile,
    const iree_hal_profile_queue_event_t* queue_event, iree_time_t start,
    iree_hal_profile_host_execution_event_flags_t flags,
    iree_status_code_t status_code, uint32_t operation_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_HAL_DRIVERS_AMD_XDNA_PROFILE_H_
