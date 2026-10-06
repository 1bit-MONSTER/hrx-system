// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/graph.h"
#include "common/stream.h"

// Captured HIP callback adapted to the HAL host-call ABI.
typedef struct iree_hal_streaming_host_callback_t {
  // User callback invoked at the stream timeline point.
  void (*fn)(void* user_data);
  // Opaque argument passed to |fn|.
  void* user_data;
} iree_hal_streaming_host_callback_t;

static iree_status_t iree_hal_streaming_host_callback_thunk(
    void* user_data, const uint64_t args[4],
    iree_hal_host_call_context_t* context) {
  iree_hal_streaming_host_callback_t* callback =
      (iree_hal_streaming_host_callback_t*)user_data;
  callback->fn(callback->user_data);
  iree_allocator_free(iree_allocator_system(), callback);
  return iree_ok_status();
}

typedef struct iree_hal_streaming_capture_host_function_t {
  void (*fn)(void* user_data);
  void* user_data;
} iree_hal_streaming_capture_host_function_t;

static iree_status_t iree_hal_streaming_capture_record_host_function(
    iree_hal_streaming_graph_t* graph,
    iree_hal_streaming_graph_node_t** dependencies,
    iree_host_size_t dependency_count, void* user_data,
    iree_hal_streaming_graph_node_t** out_terminal_node) {
  iree_hal_streaming_capture_host_function_t* capture =
      (iree_hal_streaming_capture_host_function_t*)user_data;
  return iree_hal_streaming_graph_add_host_call_node(
      graph, dependencies, dependency_count, capture->fn, capture->user_data,
      out_terminal_node);
}

iree_status_t iree_hal_streaming_queue_host_call(
    iree_hal_streaming_stream_t* stream, iree_hal_host_call_t call,
    const uint64_t args[4], iree_hal_host_call_flags_t flags) {
  IREE_ASSERT_ARGUMENT(stream);
  IREE_ASSERT_ARGUMENT(call.fn);
  IREE_TRACE_ZONE_BEGIN(z0);

  IREE_RETURN_AND_END_ZONE_IF_ERROR(z0,
                                    iree_hal_streaming_stream_flush(stream));

  iree_slim_mutex_lock(&stream->mutex);
  uint64_t wait_value = 0;
  uint64_t signal_value = 0;
  iree_status_t status = iree_hal_streaming_stream_reserve_next_value_locked(
      stream, &wait_value, &signal_value);
  if (iree_status_is_ok(status)) {
    const iree_hal_semaphore_list_t wait_semaphores = {
        .count = wait_value > 0 ? 1 : 0,
        .semaphores = &stream->timeline_semaphore,
        .payload_values = &wait_value,
    };
    const iree_hal_semaphore_list_t signal_semaphores = {
        .count = 1,
        .semaphores = &stream->timeline_semaphore,
        .payload_values = &signal_value,
    };
    status = iree_hal_queue_host_call(stream->queue, wait_semaphores,
                                      signal_semaphores, call, args, flags);
    if (iree_status_is_ok(status)) {
      stream->pending_value = signal_value;
    }
  }
  iree_slim_mutex_unlock(&stream->mutex);

  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t iree_hal_streaming_launch_host_function(
    iree_hal_streaming_stream_t* stream, void (*fn)(void*), void* user_data) {
  IREE_ASSERT_ARGUMENT(stream);
  IREE_ASSERT_ARGUMENT(fn);
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_streaming_capture_host_function_t capture = {
      .fn = fn,
      .user_data = user_data,
  };
  bool was_capturing = false;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_streaming_capture_try_record_node(
              stream, iree_hal_streaming_capture_record_host_function, &capture,
              &was_capturing));
  if (was_capturing) {
    IREE_TRACE_ZONE_END(z0);
    return iree_ok_status();
  }

  iree_hal_streaming_host_callback_t* callback = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(iree_allocator_system(), sizeof(*callback),
                                (void**)&callback));
  callback->fn = fn;
  callback->user_data = user_data;

  const uint64_t args[4] = {0, 0, 0, 0};
  const iree_hal_host_call_t call =
      iree_hal_make_host_call(iree_hal_streaming_host_callback_thunk, callback);
  iree_status_t status = iree_hal_streaming_queue_host_call(
      stream, call, args, IREE_HAL_HOST_CALL_FLAG_NONE);
  if (!iree_status_is_ok(status)) {
    iree_allocator_free(iree_allocator_system(), callback);
  }

  IREE_TRACE_ZONE_END(z0);
  return status;
}
