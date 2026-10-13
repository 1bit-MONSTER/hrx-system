// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/xdna/command_buffer.h"

#include "iree/base/threading/mutex.h"
#include "iree/hal/drivers/amd/xdna/barrier.h"
#include "iree/hal/utils/resource_set.h"

typedef struct iree_hal_amd_xdna_command_buffer_t {
  // HAL recording identity and validation state.
  iree_hal_command_buffer_t base;
  // Allocator for the wrapper and cold replay-frame growth.
  iree_allocator_t host_allocator;
  // Recording-owned fixed block pools.
  iree_hal_amd_xdna_queue_storage_t storage;
  // Immutable records and captured update bytes.
  iree_hal_amd_xdna_queue_capture_t capture;
  // Retains direct resources unless the recording is UNRETAINED.
  iree_hal_resource_set_t* resources;
  // First command, or NULL for an empty recording.
  iree_hal_amd_xdna_record_t* head;
  // Last command while recording.
  iree_hal_amd_xdna_record_t* tail;
  // Largest binding scratch required by any one dispatch.
  iree_host_size_t maximum_binding_count;
  // Total placed allocation size for each replay frame, computed at end().
  iree_host_size_t execution_size;
  // Offset to the captured table in each frame.
  iree_host_size_t table_offset;
  // Offset to native binding scratch in each frame.
  iree_host_size_t bindings_offset;
  // Protects only replay-frame free-list links.
  iree_slim_mutex_t execution_mutex;
  // Completed frames available for reuse.
  iree_hal_amd_xdna_execution_t* available;
} iree_hal_amd_xdna_command_buffer_t;

static const iree_hal_command_buffer_vtable_t
    iree_hal_amd_xdna_command_buffer_vtable;

static void iree_hal_amd_xdna_command_buffer_destroy(
    iree_hal_command_buffer_t* base) {
  iree_hal_amd_xdna_command_buffer_t* buffer = (void*)base;
  iree_hal_amd_xdna_execution_t* execution = buffer->available;
  while (execution) {
    iree_hal_amd_xdna_execution_t* next = execution->next;
    iree_allocator_free(buffer->host_allocator, execution);
    execution = next;
  }
  iree_slim_mutex_deinitialize(&buffer->execution_mutex);
  iree_hal_resource_set_free(buffer->resources);
  iree_hal_amd_xdna_queue_capture_deinitialize(&buffer->capture);
  iree_hal_amd_xdna_queue_storage_deinitialize(&buffer->storage);
  iree_allocator_free(buffer->host_allocator, buffer);
}

iree_status_t iree_hal_amd_xdna_command_buffer_create(
    const iree_hal_queue_family_t* family, iree_hal_command_buffer_mode_t mode,
    iree_hal_command_category_t categories, iree_host_size_t binding_capacity,
    iree_allocator_t host_allocator, iree_hal_command_buffer_t** out_buffer) {
  *out_buffer = NULL;
  iree_host_size_t size = 0;
  iree_host_size_t validation_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_hal_amd_xdna_command_buffer_t), &size,
      IREE_STRUCT_FIELD(
          iree_hal_command_buffer_validation_state_size(mode, binding_capacity),
          uint8_t, &validation_offset)));
  iree_hal_amd_xdna_command_buffer_t* buffer = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, size, (void**)&buffer));
  memset(buffer, 0, sizeof(*buffer));
  iree_hal_command_buffer_initialize(family, mode, categories, binding_capacity,
                                     (uint8_t*)buffer + validation_offset,
                                     &iree_hal_amd_xdna_command_buffer_vtable,
                                     &buffer->base);
  buffer->host_allocator = host_allocator;
  iree_slim_mutex_initialize(&buffer->execution_mutex);
  iree_hal_amd_xdna_queue_storage_initialize(host_allocator, &buffer->storage);
  iree_hal_amd_xdna_queue_capture_initialize(&buffer->storage,
                                             &buffer->capture);
  iree_status_t status = iree_ok_status();
  if (!iree_any_bit_set(mode, IREE_HAL_COMMAND_BUFFER_MODE_UNRETAINED)) {
    status = iree_hal_resource_set_allocate(
        &buffer->storage.metadata_block_pool, &buffer->resources);
  }
  if (iree_status_is_ok(status)) {
    *out_buffer = &buffer->base;
  } else {
    iree_hal_command_buffer_release(&buffer->base);
  }
  return status;
}

static iree_status_t iree_hal_amd_xdna_command_buffer_begin(
    iree_hal_command_buffer_t* base) {
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_xdna_command_buffer_end(
    iree_hal_command_buffer_t* base) {
  iree_hal_amd_xdna_command_buffer_t* buffer = (void*)base;
  return IREE_STRUCT_LAYOUT(
      sizeof(iree_hal_amd_xdna_execution_t), &buffer->execution_size,
      IREE_STRUCT_FIELD(base->binding_count, iree_hal_buffer_binding_t,
                        &buffer->table_offset),
      IREE_STRUCT_FIELD(buffer->maximum_binding_count,
                        iree_hal_amd_xdna_executable_binding_t,
                        &buffer->bindings_offset));
}

static iree_status_t iree_hal_amd_xdna_command_buffer_begin_debug_group(
    iree_hal_command_buffer_t* base, iree_string_view_t label,
    iree_hal_label_color_t color, const iree_hal_label_location_t* location) {
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_xdna_command_buffer_end_debug_group(
    iree_hal_command_buffer_t* base) {
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_xdna_command_buffer_barrier(
    iree_hal_command_buffer_t* base, const iree_hal_barrier_t* barrier) {
  const iree_hal_barrier_list_t list = {.count = 1, .values = barrier};
  const iree_hal_queue_barriers_t barriers = {.before = &list};
  return iree_hal_amd_xdna_queue_barriers_validate(&barriers);
}

static iree_status_t iree_hal_amd_xdna_command_buffer_atomic_wait(
    iree_hal_command_buffer_t* base, iree_hal_execution_stage_t source_stage,
    iree_hal_execution_stage_t target_stage, iree_hal_buffer_ref_t target,
    iree_hal_atomic_wait_params_t params) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "XDNA command buffers do not support atomic waits");
}

static iree_status_t iree_hal_amd_xdna_command_buffer_atomic_store(
    iree_hal_command_buffer_t* base, iree_hal_execution_stage_t source_stage,
    iree_hal_execution_stage_t target_stage, iree_hal_buffer_ref_t target,
    iree_hal_atomic_store_params_t params) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "XDNA command buffers do not support atomic stores");
}

static iree_status_t iree_hal_amd_xdna_command_buffer_atomic_rmw(
    iree_hal_command_buffer_t* base, iree_hal_execution_stage_t source_stage,
    iree_hal_execution_stage_t target_stage, iree_hal_buffer_ref_t target,
    iree_hal_atomic_rmw_params_t params) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "XDNA command buffers do not support atomic RMWs");
}

static iree_status_t iree_hal_amd_xdna_command_buffer_advise_buffer(
    iree_hal_command_buffer_t* base, iree_hal_buffer_ref_t reference,
    iree_hal_memory_advise_flags_t flags, uint64_t arg0, uint64_t arg1) {
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_xdna_command_buffer_capture_ref(
    iree_hal_amd_xdna_command_buffer_t* buffer,
    iree_hal_buffer_ref_t reference) {
  if (reference.buffer) {
    return iree_hal_resource_set_insert(buffer->resources, 1,
                                        &reference.buffer);
  }
  if (reference.buffer_slot >= buffer->base.binding_capacity) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "XDNA indirect binding slot %u exceeds capacity %u",
                            reference.buffer_slot,
                            buffer->base.binding_capacity);
  }
  buffer->base.binding_count =
      iree_max(buffer->base.binding_count, reference.buffer_slot + 1);
  return iree_ok_status();
}

static void iree_hal_amd_xdna_command_buffer_append(
    iree_hal_amd_xdna_command_buffer_t* buffer,
    iree_hal_amd_xdna_record_t* record) {
  if (buffer->tail) {
    buffer->tail->next = record;
  } else {
    buffer->head = record;
  }
  buffer->tail = record;
}

static iree_status_t iree_hal_amd_xdna_command_buffer_transfer(
    iree_hal_amd_xdna_command_buffer_t* buffer,
    iree_hal_amd_xdna_record_kind_t kind, iree_hal_buffer_ref_t source,
    iree_hal_buffer_ref_t target, const void* pattern,
    iree_host_size_t pattern_length, iree_const_byte_span_t update) {
  if (!target.length) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_xdna_command_buffer_capture_ref(buffer, target));
  if (kind == IREE_HAL_AMD_XDNA_RECORD_COPY) {
    IREE_RETURN_IF_ERROR(
        iree_hal_amd_xdna_command_buffer_capture_ref(buffer, source));
  }
  iree_hal_amd_xdna_record_t* record = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(&buffer->capture.metadata_arena,
                                           sizeof(*record), (void**)&record));
  memset(record, 0, sizeof(*record));
  record->kind = kind;
  record->transfer.source = source;
  record->transfer.target = target;
  record->transfer.pattern_length = pattern_length;
  if (pattern_length) {
    memcpy(record->transfer.pattern, pattern, pattern_length);
  }
  IREE_RETURN_IF_ERROR(iree_hal_amd_xdna_queue_capture_payload(
      &buffer->capture, update, &record->transfer.payload));
  iree_hal_amd_xdna_command_buffer_append(buffer, record);
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_xdna_command_buffer_fill_buffer(
    iree_hal_command_buffer_t* base, iree_hal_buffer_ref_t target,
    const void* pattern, iree_host_size_t pattern_length,
    iree_hal_fill_flags_t flags) {
  return iree_hal_amd_xdna_command_buffer_transfer(
      (void*)base, IREE_HAL_AMD_XDNA_RECORD_FILL, (iree_hal_buffer_ref_t){0},
      target, pattern, pattern_length, iree_const_byte_span_empty());
}

static iree_status_t iree_hal_amd_xdna_command_buffer_update_buffer(
    iree_hal_command_buffer_t* base, const void* source,
    iree_host_size_t source_offset, iree_hal_buffer_ref_t target,
    iree_hal_update_flags_t flags) {
  return iree_hal_amd_xdna_command_buffer_transfer(
      (void*)base, IREE_HAL_AMD_XDNA_RECORD_UPDATE, (iree_hal_buffer_ref_t){0},
      target, NULL, 0,
      iree_make_const_byte_span((const uint8_t*)source + source_offset,
                                target.length));
}

static iree_status_t iree_hal_amd_xdna_command_buffer_copy_buffer(
    iree_hal_command_buffer_t* base, iree_hal_buffer_ref_t source,
    iree_hal_buffer_ref_t target, iree_hal_copy_flags_t flags) {
  return iree_hal_amd_xdna_command_buffer_transfer(
      (void*)base, IREE_HAL_AMD_XDNA_RECORD_COPY, source, target, NULL, 0,
      iree_const_byte_span_empty());
}

static iree_status_t iree_hal_amd_xdna_command_buffer_dispatch(
    iree_hal_command_buffer_t* base, iree_hal_executable_t* executable,
    iree_hal_executable_function_t token,
    const iree_hal_dispatch_config_t config, iree_const_byte_span_t constants,
    iree_hal_buffer_ref_list_t bindings, iree_hal_dispatch_flags_t flags) {
  iree_hal_amd_xdna_command_buffer_t* buffer = (void*)base;
  const iree_hal_dispatch_flags_t supported_flags =
      IREE_HAL_DISPATCH_FLAG_ALLOW_INLINE_EXECUTION |
      IREE_HAL_DISPATCH_FLAG_BORROW_RESOURCE_LIFETIMES;
  if ((flags & ~supported_flags) || config.workgroup_count_ref.buffer ||
      config.dynamic_workgroup_local_memory || constants.data_length) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "XDNA dispatch requires a whole-array invocation");
  }
  for (uint32_t i = 0; i < 3; ++i) {
    if (config.workgroup_count[i] > 1 || config.workgroup_size[i] > 1) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "XDNA function is one compiled array invocation");
    }
  }
  if (!config.workgroup_count[0] || !config.workgroup_count[1] ||
      !config.workgroup_count[2]) {
    return iree_ok_status();
  }
  iree_hal_amd_xdna_function_t* function = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_xdna_executable_lookup(executable, token, &function));
  if (bindings.count != iree_hal_amd_xdna_function_binding_count(function)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "XDNA dispatch binding count does not match function");
  }
  IREE_RETURN_IF_ERROR(
      iree_hal_resource_set_insert(buffer->resources, 1, &executable));
  iree_host_size_t size = 0;
  iree_host_size_t bindings_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_hal_amd_xdna_record_t), &size,
      IREE_STRUCT_FIELD(bindings.count, iree_hal_amd_xdna_executable_binding_t,
                        &bindings_offset)));
  iree_hal_amd_xdna_record_t* record = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(&buffer->capture.metadata_arena,
                                           size, (void**)&record));
  memset(record, 0, size);
  record->kind = IREE_HAL_AMD_XDNA_RECORD_DISPATCH;
  record->dispatch.function = function;
  record->dispatch.binding_count = bindings.count;
  record->dispatch.bindings = (void*)((uint8_t*)record + bindings_offset);
  for (uint32_t i = 0; i < bindings.count; ++i) {
    if (!iree_hal_amd_xdna_function_uses_binding(function, i)) {
      continue;
    }
    IREE_RETURN_IF_ERROR(iree_hal_amd_xdna_command_buffer_capture_ref(
        buffer, bindings.values[i]));
    if (bindings.values[i].buffer) {
      IREE_RETURN_IF_ERROR(iree_hal_amd_xdna_function_capture_binding(
          function, base->queue_family, i, bindings.values[i],
          &record->dispatch.bindings[i]));
    } else {
      record->dispatch.bindings[i].buffer_ref = bindings.values[i];
    }
  }
  buffer->maximum_binding_count =
      iree_max(buffer->maximum_binding_count, bindings.count);
  iree_hal_amd_xdna_command_buffer_append(buffer, record);
  return iree_ok_status();
}

iree_status_t iree_hal_amd_xdna_command_buffer_acquire_execution(
    iree_hal_command_buffer_t* base, iree_hal_buffer_binding_table_t table,
    iree_hal_queue_execute_flags_t flags,
    iree_hal_amd_xdna_execution_t** out_execution) {
  *out_execution = NULL;
  iree_hal_amd_xdna_command_buffer_t* buffer = (void*)base;
  if (table.count < base->binding_count) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "XDNA execution requires %u indirect bindings",
                            base->binding_count);
  }
  iree_slim_mutex_lock(&buffer->execution_mutex);
  iree_hal_amd_xdna_execution_t* execution = buffer->available;
  if (execution) {
    buffer->available = execution->next;
  }
  iree_slim_mutex_unlock(&buffer->execution_mutex);
  if (!execution) {
    IREE_RETURN_IF_ERROR(iree_allocator_malloc(
        buffer->host_allocator, buffer->execution_size, (void**)&execution));
  }
  memset(execution, 0, sizeof(*execution));
  execution->owner = base;
  execution->cursor = buffer->head;
  execution->bindings = (void*)((uint8_t*)execution + buffer->bindings_offset);
  execution->flags = flags;
  iree_hal_buffer_binding_t* captured =
      (void*)((uint8_t*)execution + buffer->table_offset);
  execution->table =
      (iree_hal_buffer_binding_table_t){base->binding_count, captured};
  for (iree_host_size_t i = 0; i < base->binding_count; ++i) {
    captured[i] = table.bindings[i];
    if (!iree_any_bit_set(
            flags, IREE_HAL_QUEUE_EXECUTE_FLAG_BORROW_BINDING_TABLE_LIFETIME)) {
      iree_hal_buffer_retain(captured[i].buffer);
    }
  }
  iree_hal_command_buffer_retain(base);
  *out_execution = execution;
  return iree_ok_status();
}

void iree_hal_amd_xdna_execution_release(
    iree_hal_amd_xdna_execution_t* execution) {
  if (!execution) {
    return;
  }
  iree_hal_amd_xdna_command_buffer_t* buffer = (void*)execution->owner;
  if (!iree_any_bit_set(
          execution->flags,
          IREE_HAL_QUEUE_EXECUTE_FLAG_BORROW_BINDING_TABLE_LIFETIME)) {
    for (iree_host_size_t i = 0; i < execution->table.count; ++i) {
      iree_hal_buffer_release(execution->table.bindings[i].buffer);
    }
  }
  iree_slim_mutex_lock(&buffer->execution_mutex);
  execution->next = buffer->available;
  buffer->available = execution;
  iree_slim_mutex_unlock(&buffer->execution_mutex);
  iree_hal_command_buffer_release(&buffer->base);
}

iree_status_t iree_hal_amd_xdna_execution_prepare_dispatch(
    iree_hal_amd_xdna_execution_t* execution,
    iree_hal_amd_xdna_invocation_t** out_invocation,
    amdf_xdna_kernel_command_t* out_command) {
  const iree_hal_amd_xdna_record_t* record = execution->cursor;
  memcpy(execution->bindings, record->dispatch.bindings,
         record->dispatch.binding_count * sizeof(*execution->bindings));
  for (uint32_t i = 0; i < record->dispatch.binding_count; ++i) {
    if (execution->bindings[i].buffer_ref.buffer ||
        !iree_hal_amd_xdna_function_uses_binding(record->dispatch.function,
                                                 i)) {
      continue;
    }
    iree_hal_buffer_ref_t reference;
    IREE_RETURN_IF_ERROR(iree_hal_buffer_binding_table_resolve_ref(
        execution->table, execution->bindings[i].buffer_ref, &reference));
    IREE_RETURN_IF_ERROR(iree_hal_amd_xdna_function_capture_binding(
        record->dispatch.function, execution->owner->queue_family, i, reference,
        &execution->bindings[i]));
  }
  return iree_hal_amd_xdna_function_prepare(record->dispatch.function,
                                            execution->bindings, out_invocation,
                                            out_command);
}

static const iree_hal_command_buffer_vtable_t
    iree_hal_amd_xdna_command_buffer_vtable = {
        .destroy = iree_hal_amd_xdna_command_buffer_destroy,
        .begin = iree_hal_amd_xdna_command_buffer_begin,
        .end = iree_hal_amd_xdna_command_buffer_end,
        .begin_debug_group = iree_hal_amd_xdna_command_buffer_begin_debug_group,
        .end_debug_group = iree_hal_amd_xdna_command_buffer_end_debug_group,
        .barrier = iree_hal_amd_xdna_command_buffer_barrier,
        .atomic_wait = iree_hal_amd_xdna_command_buffer_atomic_wait,
        .atomic_store = iree_hal_amd_xdna_command_buffer_atomic_store,
        .atomic_rmw = iree_hal_amd_xdna_command_buffer_atomic_rmw,
        .advise_buffer = iree_hal_amd_xdna_command_buffer_advise_buffer,
        .fill_buffer = iree_hal_amd_xdna_command_buffer_fill_buffer,
        .update_buffer = iree_hal_amd_xdna_command_buffer_update_buffer,
        .copy_buffer = iree_hal_amd_xdna_command_buffer_copy_buffer,
        .dispatch = iree_hal_amd_xdna_command_buffer_dispatch,
};
