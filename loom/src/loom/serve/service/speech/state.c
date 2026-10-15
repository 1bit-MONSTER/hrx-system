// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/speech/state.h"

void loom_serve_speech_state_initialize(iree_allocator_t host_allocator,
                                        loom_serve_speech_state_t* out_state) {
  *out_state = (loom_serve_speech_state_t){.host_allocator = host_allocator};
  loom_serve_retirement_initialize(&out_state->retirement);
}

iree_status_t loom_serve_speech_state_allocate(
    loom_serve_speech_state_t* state, loom_serve_device_t* device,
    iree_host_size_t row_count, iree_device_size_t input_stride,
    iree_device_size_t stream_stride, iree_device_size_t conditioning_length,
    iree_device_size_t alignment) {
  const iree_allocator_t host_allocator = state->host_allocator;
  state->pool = loom_serve_device_memory_pool(device);
  state->row_count = row_count;
  state->strides[LOOM_SERVE_SPEECH_STATE_INPUT] = input_stride;
  state->strides[LOOM_SERVE_SPEECH_STATE_CONDITIONING] = conditioning_length;
  state->strides[LOOM_SERVE_SPEECH_STATE_STREAM] = stream_stride;
  iree_status_t status = iree_allocator_malloc_array(
      host_allocator, row_count, sizeof(*state->rows), (void**)&state->rows);
  for (iree_host_size_t i = 0;
       i < LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT && iree_status_is_ok(status);
       ++i) {
    const iree_device_size_t length =
        state->strides[i] *
        (i == LOOM_SERVE_SPEECH_STATE_CONDITIONING ? 1 : row_count);
    if (state->pool) {
      status = loom_serve_virtual_buffer_allocate(state->pool, length,
                                                  alignment, &state->statistics,
                                                  &state->reservations[i]);
      if (iree_status_is_ok(status)) {
        state->buffers[i] =
            loom_serve_virtual_buffer_handle(state->reservations[i]);
        iree_hal_buffer_retain(state->buffers[i]);
      }
    } else {
      status = iree_hal_pool_allocate_buffer(
          loom_serve_device_storage_pool(device),
          iree_hal_make_pool_reservation_request(length, alignment),
          iree_infinite_timeout(), &state->buffers[i]);
    }
    if (iree_status_is_ok(status)) {
      status = loom_serve_retirement_track(&state->retirement,
                                           &state->buffers[i], host_allocator);
    }
  }
  return status;
}

iree_hal_buffer_t* loom_serve_speech_state_buffer(
    const loom_serve_speech_state_t* state, iree_host_size_t index) {
  return state->buffers[index];
}

iree_status_t loom_serve_speech_state_try_reserve(
    loom_serve_speech_state_t* state, iree_host_size_t row_index,
    bool* out_reserved) {
  *out_reserved = false;
  bool reserved = true;
  if (state->pool) {
    loom_serve_memory_range_t ranges[LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT];
    for (iree_host_size_t i = 0; i < LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT;
         ++i) {
      ranges[i] = (loom_serve_memory_range_t){
          .buffer = state->reservations[i],
          .offset = i == LOOM_SERVE_SPEECH_STATE_CONDITIONING
                        ? 0
                        : row_index * state->strides[i],
          .length = state->strides[i],
      };
    }
    IREE_RETURN_IF_ERROR(loom_serve_memory_pool_try_commit(
        state->pool, IREE_ARRAYSIZE(ranges), ranges, &reserved));
  }
  if (reserved) {
    state->rows[row_index] = true;
  }
  *out_reserved = reserved;
  return iree_ok_status();
}

iree_status_t loom_serve_speech_state_release_row(
    loom_serve_speech_state_t* state, iree_host_size_t row_index) {
  state->rows[row_index] = false;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       state->pool && i < LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT &&
       iree_status_is_ok(status);
       ++i) {
    if (i == LOOM_SERVE_SPEECH_STATE_CONDITIONING) {
      continue;
    }
    loom_serve_virtual_buffer_begin_trim(state->reservations[i]);
    for (iree_host_size_t row = 0; row < state->row_count; ++row) {
      if (state->rows[row]) {
        loom_serve_virtual_buffer_keep(
            state->reservations[i], row * state->strides[i], state->strides[i]);
      }
    }
    status = loom_serve_virtual_buffer_trim(state->reservations[i]);
  }
  return status;
}

void loom_serve_speech_state_retire(loom_serve_speech_state_t* state,
                                    void (*wake)(void* user_data),
                                    void* user_data) {
  loom_serve_retirement_set_observer(&state->retirement, wake, user_data);
  for (iree_host_size_t i = 0; i < LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT; ++i) {
    iree_hal_buffer_release(state->buffers[i]);
    state->buffers[i] = NULL;
  }
}

bool loom_serve_speech_state_is_retired(loom_serve_speech_state_t* state) {
  return loom_serve_retirement_is_idle(&state->retirement);
}

iree_status_t loom_serve_speech_state_deinitialize(
    loom_serve_speech_state_t* state) {
  IREE_ASSERT(loom_serve_speech_state_is_retired(state));
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < LOOM_SERVE_SPEECH_STATE_BUFFER_COUNT && iree_status_is_ok(status);
       ++i) {
    status = loom_serve_virtual_buffer_free(state->reservations[i]);
    if (iree_status_is_ok(status)) {
      state->reservations[i] = NULL;
    }
  }
  if (iree_status_is_ok(status)) {
    loom_serve_retirement_deinitialize(&state->retirement);
    iree_allocator_free(state->host_allocator, state->rows);
    state->rows = NULL;
  }
  return status;
}
