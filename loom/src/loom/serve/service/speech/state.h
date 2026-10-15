// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_SPEECH_STATE_H_
#define LOOM_SERVE_SERVICE_SPEECH_STATE_H_

#include "loom/serve/runtime/device/device.h"
#include "loom/serve/runtime/execution/retirement.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LOOM_SERVE_SPEECH_STATE_INPUT = 0,
  LOOM_SERVE_SPEECH_STATE_CONDITIONING = 1,
  LOOM_SERVE_SPEECH_STATE_STREAM = 2,
  LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT = 3,
};

// Stable source-addressed input/history rows and shared conditioning. Batch
// membership changes only the work packet, never these roots or row origins.
typedef struct loom_serve_speech_state_t {
  // Allocator owning the row ownership array.
  iree_allocator_t host_allocator;
  // Borrowed shared commitment owner; NULL selects fixed backing.
  loom_serve_memory_pool_t* pool;
  // Accounting shared by all virtual reservations below.
  loom_serve_memory_statistics_t statistics;
  // Final queue ownership, independent of success/failure readiness.
  loom_serve_retirement_t retirement;
  // Whole-arena roots lent to numerical commands and transfers.
  iree_hal_buffer_t* buffers[LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT];
  // Optional stable address reservations, owned through actual retirement.
  loom_serve_virtual_buffer_t*
      reservations[LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT];
  // Input/state row strides and the complete conditioning extent.
  iree_device_size_t strides[LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT];
  // Addressable row count, independent of current cohort membership.
  iree_host_size_t row_count;
  // Owner-only row reservations, retained until release_row.
  bool* rows;
} loom_serve_speech_state_t;

void loom_serve_speech_state_initialize(iree_allocator_t host_allocator,
                                        loom_serve_speech_state_t* out_state);

// Geometry is validated by the source-loading boundary. Input and stream
// strides are positive alignment multiples; row_count * stride fits the device
// address domain.
// Elastic backing reserves addresses only; fixed backing allocates all rows.
// The shared device and physical pool outlive state. On failure, state owns
// partial construction and remains safe to retire and deinitialize.
iree_status_t loom_serve_speech_state_allocate(
    loom_serve_speech_state_t* state, loom_serve_device_t* device,
    iree_host_size_t row_count, iree_device_size_t input_stride,
    iree_device_size_t stream_stride, iree_device_size_t conditioning_length,
    iree_device_size_t alignment);

// Borrows a tracked whole-arena root until state retirement. The index names
// one of the state buffers above; row offsets remain source-owned facts.
iree_hal_buffer_t* loom_serve_speech_state_buffer(
    const loom_serve_speech_state_t* state, iree_host_size_t index);

// At a retired model cut, admits one unowned row and the conditioning bank in
// one physical commitment transaction. Parameter pins already protect needed
// weights. A false result owns no row and commits no new destination backing.
// Actual platform failures retain any partial backing for terminal teardown.
iree_status_t loom_serve_speech_state_try_reserve(
    loom_serve_speech_state_t* state, iree_host_size_t row_index,
    bool* out_reserved);

// Releases a reserved row only after its accepted work and host borrowers
// retire. Surviving rows retain their contents and addresses. Shared slabs stay
// backed while any row needs them; conditioning remains reusable model state.
iree_status_t loom_serve_speech_state_release_row(
    loom_serve_speech_state_t* state, iree_host_size_t row_index);

// Relinquishes caller roots after all CPU borrowers stop. Accepted queues can
// still retain tracked views. The observer only schedules the serialized owner.
void loom_serve_speech_state_retire(loom_serve_speech_state_t* state,
                                    void (*wake)(void* user_data),
                                    void* user_data);
bool loom_serve_speech_state_is_retired(loom_serve_speech_state_t* state);

// Requires actual retirement, independently of failed semaphore readiness.
// A platform release failure preserves remaining reservations for retry.
iree_status_t loom_serve_speech_state_deinitialize(
    loom_serve_speech_state_t* state);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_SPEECH_STATE_H_
