// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/task/queue/queue.h"
#include "iree/hal/virtual_buffer.h"

static void iree_hal_task_queue_map_execute(
    iree_hal_memory_maintenance_entry_t* entry) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_hal_task_queue_op_t* operation =
      iree_containerof(entry, iree_hal_task_queue_op_t, mapping.entry);
  iree_hal_host_call_context_t context = {
      .queue = &operation->queue->base,
      .signal_semaphore_list = iree_hal_semaphore_list_empty(),
  };
  const uint64_t args[4] = {0};
  iree_status_t status = operation->mapping.call.fn(
      operation->mapping.call.user_data, args, &context);
  if (iree_status_is_ok(status)) {
    iree_hal_task_queue_op_complete(operation);
  } else {
    iree_hal_task_queue_op_fail(operation, status);
  }
  IREE_TRACE_ZONE_END(z0);
}

void iree_hal_task_queue_issue_map(iree_hal_task_queue_op_t* operation) {
  operation->mapping.entry.fn = iree_hal_task_queue_map_execute;
  iree_hal_memory_maintenance_enqueue(operation->queue->memory_maintenance,
                                      &operation->mapping.entry);
}

iree_status_t iree_hal_task_queue_submit_map(
    iree_hal_queue_t* base_queue, iree_hal_semaphore_list_t waits,
    iree_hal_semaphore_list_t signals, iree_host_size_t mapping_count,
    const iree_hal_memory_mapping_t* mappings,
    iree_hal_queue_map_flags_t flags) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_hal_task_queue_t* queue = (iree_hal_task_queue_t*)base_queue;
  iree_hal_host_call_t call;
  iree_allocator_t host_allocator = iree_hal_device_host_allocator(
      iree_hal_queue_family_device(iree_hal_queue_family(base_queue)));
  iree_status_t status = iree_hal_memory_mapping_prepare(
      base_queue, mapping_count, mappings, flags, host_allocator, &call);
  if (iree_status_is_ok(status)) {
    iree_hal_task_queue_op_t* operation = NULL;
    status = iree_hal_task_queue_submit_op_begin(
        queue, IREE_HAL_TASK_QUEUE_OP_MAP, &signals, &operation);
    if (iree_status_is_ok(status)) {
      operation->mapping.call = call;
      status = iree_hal_task_queue_submit_op_finish(
          operation, waits, call.resource ? 1 : 0, &call.resource);
    }
    iree_hal_resource_release(call.resource);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}
