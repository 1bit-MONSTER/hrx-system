// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_REPLAY_RECORDER_POOL_H_
#define IREE_HAL_REPLAY_RECORDER_POOL_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/hal/replay/format.h"
#include "iree/hal/replay/recorder.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Serializes validated scope families in canonical group coordinates.
void iree_hal_replay_recorder_pool_capture_families(
    const iree_hal_pool_scope_t* scope,
    iree_hal_replay_pool_family_access_payload_t* family_payloads);

// Records one complete slab-pool construction and wraps the returned pool.
iree_status_t iree_hal_replay_recorder_create_slab_pool(
    iree_hal_replay_recorder_t* recorder,
    iree_hal_slab_pool_create_callback_t next,
    iree_hal_device_group_t* request_group, const iree_hal_pool_scope_t* scope,
    const iree_hal_slab_pool_options_t* options,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_pool);

// Returns the captured object ID when |pool| belongs to |recorder|.
iree_hal_replay_object_id_t iree_hal_replay_recorder_find_pool_id(
    iree_hal_replay_recorder_t* recorder, iree_hal_pool_t* pool);

// Returns the wrapped native pool, or |pool| when it is not a replay proxy.
iree_hal_pool_t* iree_hal_replay_recorder_pool_base_or_self(
    iree_hal_pool_t* pool);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_REPLAY_RECORDER_POOL_H_
