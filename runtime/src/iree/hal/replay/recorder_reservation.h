// Copyright 2026 The IREE Authors
//
// Licensed under the Apache v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_REPLAY_RECORDER_RESERVATION_H_
#define IREE_HAL_REPLAY_RECORDER_RESERVATION_H_

#include "iree/hal/api.h"
#include "iree/hal/replay/recorder.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Records address ownership independently from the stable execution view.
iree_status_t iree_hal_replay_recorder_create_memory_reservation(
    iree_hal_replay_recorder_t* recorder,
    iree_hal_memory_reservation_create_callback_t next,
    iree_hal_device_group_t* request_group, const iree_hal_pool_scope_t* scope,
    const iree_hal_memory_reservation_options_t* options,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t** out_reservation);

// Returns the ordinary recording proxy for a reservation's stable view, or
// |buffer| unchanged. Subspan callers first resolve their allocated buffer.
iree_hal_buffer_t* iree_hal_replay_recorder_reservation_buffer_proxy_or_self(
    iree_hal_buffer_t* buffer);

// Captures one complete mapping batch and forwards native owners and sources.
iree_status_t iree_hal_replay_recorder_queue_map(
    iree_hal_replay_recorder_t* recorder, iree_hal_replay_object_id_t device_id,
    iree_hal_replay_object_id_t queue_id, iree_hal_queue_t* base_queue,
    iree_hal_semaphore_list_t wait_semaphore_list,
    iree_hal_semaphore_list_t signal_semaphore_list,
    iree_host_size_t mapping_count, const iree_hal_memory_mapping_t* mappings,
    iree_hal_queue_map_flags_t flags, iree_allocator_t host_allocator);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_REPLAY_RECORDER_RESERVATION_H_
