// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/replay/execute_vmm.h"

#include <inttypes.h>
#include <string.h>

#include "iree/hal/replay/execute_pool.h"

static iree_status_t iree_hal_replay_executor_memory_reservation_create(
    iree_hal_replay_executor_t* executor,
    const iree_hal_replay_file_record_t* record) {
  IREE_RETURN_IF_ERROR(iree_hal_replay_executor_require_payload(
      record, IREE_HAL_REPLAY_PAYLOAD_TYPE_MEMORY_RESERVATION_CREATE,
      sizeof(iree_hal_replay_memory_reservation_create_payload_t)));
  iree_hal_replay_memory_reservation_create_payload_t payload;
  memcpy(&payload, record->payload.data, sizeof(payload));
  iree_host_size_t family_payload_length = 0;
  iree_host_size_t payload_length = 0;
  if (IREE_UNLIKELY(payload.reserved0 || payload.host_access > UINT16_MAX ||
                    payload.family_count > IREE_HOST_SIZE_MAX ||
                    !iree_host_size_checked_mul(
                        (iree_host_size_t)payload.family_count,
                        sizeof(iree_hal_replay_pool_family_access_payload_t),
                        &family_payload_length) ||
                    !iree_host_size_checked_add(sizeof(payload),
                                                family_payload_length,
                                                &payload_length) ||
                    payload_length != record->payload.data_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay reservation payload is invalid");
  }
  iree_hal_pool_family_access_t* families = NULL;
  if (payload.family_count) {
    IREE_RETURN_IF_ERROR(iree_allocator_malloc_array(
        executor->host_allocator, (iree_host_size_t)payload.family_count,
        sizeof(*families), (void**)&families));
  }
  iree_status_t status = iree_hal_replay_executor_pool_decode_families(
      executor, (iree_host_size_t)payload.family_count,
      record->payload.data + sizeof(payload), families);
  const iree_hal_pool_scope_t scope = {
      .family_count = (iree_host_size_t)payload.family_count,
      .families = families,
      .host =
          {
              .access = (iree_hal_memory_access_t)payload.host_access,
              .modes = (iree_hal_mapping_mode_t)payload.host_mapping_modes,
              .cacheability =
                  (iree_hal_host_cacheability_t)payload.host_cacheability,
          },
  };
  const iree_hal_memory_reservation_options_t options = {
      .byte_length = payload.byte_length,
      .min_alignment = payload.min_alignment,
      .mapping_granularity = payload.mapping_granularity,
  };
  iree_hal_memory_reservation_t* reservation = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_hal_memory_reservation_create(
        executor->device_group, scope, &options, executor->host_allocator,
        &reservation);
  }
  if (iree_status_is_ok(status)) {
    iree_hal_buffer_t* buffer = iree_hal_memory_reservation_buffer(reservation);
    iree_hal_buffer_retain(buffer);
    status = iree_hal_replay_executor_store(
        executor, record->header.related_object_id,
        IREE_HAL_REPLAY_OBJECT_TYPE_MEMORY_RESERVATION,
        (iree_hal_replay_object_entry_t){.value.memory_reservation =
                                             reservation});
    if (iree_status_is_ok(status)) {
      status = iree_hal_replay_executor_store(
          executor, payload.buffer_id, IREE_HAL_REPLAY_OBJECT_TYPE_BUFFER,
          (iree_hal_replay_object_entry_t){.value.buffer = buffer});
    } else {
      iree_hal_buffer_release(buffer);
    }
  }
  iree_allocator_free(executor->host_allocator, families);
  return status;
}

static iree_status_t iree_hal_replay_executor_memory_reservation_destroy(
    iree_hal_replay_executor_t* executor,
    const iree_hal_replay_file_record_t* record) {
  IREE_RETURN_IF_ERROR(iree_hal_replay_executor_require_payload(
      record, IREE_HAL_REPLAY_PAYLOAD_TYPE_NONE, 0));
  if (record->payload.data_length) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "reservation destruction requires an empty payload");
  }
  iree_hal_replay_object_entry_t* entry = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_replay_executor_lookup(
      executor, record->header.object_id,
      IREE_HAL_REPLAY_OBJECT_TYPE_MEMORY_RESERVATION, &entry));
  IREE_RETURN_IF_ERROR(
      iree_hal_replay_executor_drain_queue_completions(executor));
  iree_hal_memory_reservation_t* reservation = entry->value.memory_reservation;
  // Native destruction consumes ownership on every outcome.
  memset(entry, 0, sizeof(*entry));
  return iree_hal_memory_reservation_destroy(reservation);
}

static iree_status_t iree_hal_replay_executor_queue_map(
    iree_hal_replay_executor_t* executor,
    const iree_hal_replay_file_record_t* record) {
  IREE_RETURN_IF_ERROR(iree_hal_replay_executor_require_payload(
      record, IREE_HAL_REPLAY_PAYLOAD_TYPE_QUEUE_MAP,
      sizeof(iree_hal_replay_queue_map_payload_t)));
  iree_hal_replay_queue_map_payload_t payload;
  memcpy(&payload, record->payload.data, sizeof(payload));
  iree_host_size_t mapping_payload_length = 0;
  if (IREE_UNLIKELY(payload.mapping_count > IREE_HOST_SIZE_MAX ||
                    !iree_host_size_checked_mul(
                        (iree_host_size_t)payload.mapping_count,
                        sizeof(iree_hal_replay_memory_mapping_payload_t),
                        &mapping_payload_length))) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay mapping batch header is invalid");
  }
  iree_hal_replay_object_entry_t* queue_entry = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_replay_executor_lookup(
      executor, record->header.object_id, IREE_HAL_REPLAY_OBJECT_TYPE_QUEUE,
      &queue_entry));
  iree_hal_replay_object_entry_t* device_entry = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_replay_executor_lookup(
      executor, record->header.device_id, IREE_HAL_REPLAY_OBJECT_TYPE_DEVICE,
      &device_entry));
  iree_hal_replay_semaphore_list_storage_t waits = {0};
  iree_hal_replay_semaphore_list_storage_t signals = {0};
  iree_const_byte_span_t mapping_data;
  IREE_RETURN_IF_ERROR(iree_hal_replay_executor_make_queue_semaphore_lists(
      executor, record, sizeof(payload), payload.wait_semaphore_count,
      payload.signal_semaphore_count, mapping_payload_length, &waits, &signals,
      &mapping_data));
  iree_hal_memory_mapping_t* mappings = NULL;
  iree_status_t status = iree_ok_status();
  if (payload.mapping_count) {
    status = iree_allocator_malloc_array(
        executor->host_allocator, (iree_host_size_t)payload.mapping_count,
        sizeof(*mappings), (void**)&mappings);
  }
  for (iree_host_size_t i = 0;
       i < payload.mapping_count && iree_status_is_ok(status); ++i) {
    iree_hal_replay_memory_mapping_payload_t mapping;
    memcpy(&mapping, mapping_data.data + i * sizeof(mapping), sizeof(mapping));
    if (IREE_UNLIKELY(mapping.reserved0 || mapping.access > UINT16_MAX ||
                      mapping.source_ref.reserved0 ||
                      mapping.source_ref.buffer_slot ||
                      mapping.source_ref.length != mapping.byte_length)) {
      status = iree_make_status(IREE_STATUS_DATA_LOSS,
                                "replay mapping descriptor is invalid");
    }
    iree_hal_replay_object_entry_t* reservation_entry = NULL;
    if (iree_status_is_ok(status)) {
      status = iree_hal_replay_executor_lookup(
          executor, mapping.reservation_id,
          IREE_HAL_REPLAY_OBJECT_TYPE_MEMORY_RESERVATION, &reservation_entry);
    }
    iree_hal_replay_object_entry_t* source_entry = NULL;
    if (iree_status_is_ok(status) && mapping.source_ref.buffer_id) {
      status = iree_hal_replay_executor_lookup(
          executor, mapping.source_ref.buffer_id,
          IREE_HAL_REPLAY_OBJECT_TYPE_BUFFER, &source_entry);
    }
    if (iree_status_is_ok(status)) {
      mappings[i] = (iree_hal_memory_mapping_t){
          .reservation = reservation_entry->value.memory_reservation,
          .scope =
              {
                  .domain = iree_hal_device_group_memory_domain(
                      executor->device_group),
                  .id = mapping.scope_id,
              },
          .target_range = iree_hal_make_buffer_range(mapping.target_offset,
                                                     mapping.byte_length),
          .source = source_entry ? source_entry->value.buffer : NULL,
          .source_offset = mapping.source_ref.offset,
          .access = (iree_hal_memory_access_t)mapping.access,
          .operation = (iree_hal_memory_mapping_operation_t)mapping.operation,
      };
    }
  }
  iree_hal_replay_queue_completion_t* completion = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_hal_replay_executor_prepare_queue_completion(
        executor, device_entry->value.device, queue_entry->value.queue,
        &signals, &completion);
  }
  if (iree_status_is_ok(status)) {
    status =
        iree_hal_queue_map(queue_entry->value.queue, waits.list, signals.list,
                           (iree_host_size_t)payload.mapping_count, mappings,
                           (iree_hal_queue_map_flags_t)payload.flags);
  }
  status = iree_hal_replay_executor_finalize_queue_completion(
      executor, completion, /*flush_queue=*/true, status);
  iree_allocator_free(executor->host_allocator, mappings);
  iree_hal_replay_semaphore_list_storage_deinitialize(&signals,
                                                      executor->host_allocator);
  iree_hal_replay_semaphore_list_storage_deinitialize(&waits,
                                                      executor->host_allocator);
  return status;
}

iree_status_t iree_hal_replay_executor_replay_vmm_operation(
    iree_hal_replay_executor_t* executor,
    const iree_hal_replay_file_record_t* record) {
  switch (record->header.operation_code) {
    case IREE_HAL_REPLAY_OPERATION_CODE_MEMORY_RESERVATION_CREATE:
      return iree_hal_replay_executor_memory_reservation_create(executor,
                                                                record);
    case IREE_HAL_REPLAY_OPERATION_CODE_MEMORY_RESERVATION_DESTROY:
      return iree_hal_replay_executor_memory_reservation_destroy(executor,
                                                                 record);
    case IREE_HAL_REPLAY_OPERATION_CODE_QUEUE_MAP:
      return iree_hal_replay_executor_queue_map(executor, record);
    default:
      return iree_make_status(
          IREE_STATUS_UNIMPLEMENTED,
          "replay VMM operation %s is not implemented",
          iree_hal_replay_operation_code_string(record->header.operation_code));
  }
}
