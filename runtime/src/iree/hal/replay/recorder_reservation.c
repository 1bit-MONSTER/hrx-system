// Copyright 2026 The IREE Authors
//
// Licensed under the Apache v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/replay/recorder_reservation.h"

#include <string.h>

#include "iree/hal/replay/recorder_buffer.h"
#include "iree/hal/replay/recorder_pool.h"
#include "iree/hal/replay/recorder_record.h"

typedef struct iree_hal_replay_recorder_reservation_t {
  // Explicit address owner and independently retained stable view.
  iree_hal_memory_reservation_t base;
  // Native owner consumed by close; its metadata survives through buffer_proxy.
  iree_hal_memory_reservation_t* native_reservation;
  // Ordinary buffer recorder owning native view metadata and host-write
  // capture.
  iree_hal_buffer_t* buffer_proxy;
  // Recorder borrowed from buffer_proxy for this wrapper's lifetime.
  iree_hal_replay_recorder_t* recorder;
  // Session-local identity of the explicit address owner.
  iree_hal_replay_object_id_t reservation_id;
} iree_hal_replay_recorder_reservation_t;

static const iree_hal_buffer_vtable_t
    iree_hal_replay_recorder_reservation_buffer_vtable;

iree_hal_buffer_t* iree_hal_replay_recorder_reservation_buffer_proxy_or_self(
    iree_hal_buffer_t* buffer) {
  if (!iree_hal_resource_is(
          buffer, &iree_hal_replay_recorder_reservation_buffer_vtable)) {
    return buffer;
  }
  return ((iree_hal_replay_recorder_reservation_t*)buffer)->buffer_proxy;
}

static iree_status_t iree_hal_replay_recorder_reservation_close(
    iree_hal_memory_reservation_t* base_reservation) {
  iree_hal_replay_recorder_reservation_t* reservation =
      (iree_hal_replay_recorder_reservation_t*)base_reservation;
  iree_hal_replay_pending_record_t pending_record = {0};
  iree_hal_replay_recorder_try_begin_operation(
      reservation->recorder, IREE_HAL_REPLAY_OBJECT_ID_NONE,
      reservation->reservation_id, IREE_HAL_REPLAY_OBJECT_ID_NONE,
      IREE_HAL_REPLAY_OBJECT_TYPE_MEMORY_RESERVATION,
      IREE_HAL_REPLAY_OPERATION_CODE_MEMORY_RESERVATION_DESTROY,
      IREE_HAL_REPLAY_PAYLOAD_TYPE_NONE, &pending_record);
  // Consuming destruction reaches the native owner even after capture fails.
  iree_status_t status =
      iree_hal_memory_reservation_destroy(reservation->native_reservation);
  reservation->native_reservation = NULL;
  return iree_hal_replay_recorder_end_passthrough_operation_with_payload(
      &pending_record, status, 0, NULL);
}

static void iree_hal_replay_recorder_reservation_destroy(
    iree_hal_memory_reservation_t* base_reservation) {
  iree_hal_replay_recorder_reservation_t* reservation =
      (iree_hal_replay_recorder_reservation_t*)base_reservation;
  iree_allocator_t host_allocator = base_reservation->host_allocator;
  iree_hal_buffer_release(reservation->buffer_proxy);
  iree_allocator_free(host_allocator, reservation);
}

static void iree_hal_replay_recorder_reservation_buffer_destroy(
    iree_hal_buffer_t* buffer) {
  iree_hal_memory_reservation_t* reservation =
      (iree_hal_memory_reservation_t*)buffer;
  IREE_ASSERT(reservation->closed);
  iree_hal_memory_contract_release(buffer->memory.contract);
  iree_hal_replay_recorder_reservation_destroy(reservation);
}

static const iree_hal_buffer_vtable_t*
iree_hal_replay_recorder_reservation_proxy_vtable(iree_hal_buffer_t* proxy) {
  return (const iree_hal_buffer_vtable_t*)proxy->resource.vtable;
}

static iree_status_t iree_hal_replay_recorder_reservation_export_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length, iree_hal_external_storage_class_t storage_class,
    iree_hal_external_storage_t* out_storage,
    iree_hal_buffer_range_t* out_logical_range) {
  iree_hal_buffer_t* proxy =
      ((iree_hal_replay_recorder_reservation_t*)buffer)->buffer_proxy;
  return iree_hal_replay_recorder_reservation_proxy_vtable(proxy)->export_range(
      proxy, offset, length, storage_class, out_storage, out_logical_range);
}

static iree_status_t iree_hal_replay_recorder_reservation_map_range(
    iree_hal_buffer_t* buffer, iree_hal_mapping_mode_t mode,
    iree_hal_memory_access_t access, iree_hal_buffer_map_flags_t flags,
    iree_device_size_t offset, iree_device_size_t length,
    iree_hal_buffer_mapping_t* mapping) {
  iree_hal_buffer_t* proxy =
      ((iree_hal_replay_recorder_reservation_t*)buffer)->buffer_proxy;
  return iree_hal_replay_recorder_reservation_proxy_vtable(proxy)->map_range(
      proxy, mode, access, flags, offset, length, mapping);
}

static iree_status_t iree_hal_replay_recorder_reservation_unmap_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length, iree_hal_buffer_mapping_t* mapping) {
  iree_hal_buffer_t* proxy =
      ((iree_hal_replay_recorder_reservation_t*)buffer)->buffer_proxy;
  return iree_hal_replay_recorder_reservation_proxy_vtable(proxy)->unmap_range(
      proxy, offset, length, mapping);
}

static iree_status_t iree_hal_replay_recorder_reservation_invalidate_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  iree_hal_buffer_t* proxy =
      ((iree_hal_replay_recorder_reservation_t*)buffer)->buffer_proxy;
  return iree_hal_replay_recorder_reservation_proxy_vtable(proxy)
      ->invalidate_range(proxy, offset, length);
}

static iree_status_t iree_hal_replay_recorder_reservation_flush_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  iree_hal_buffer_t* proxy =
      ((iree_hal_replay_recorder_reservation_t*)buffer)->buffer_proxy;
  return iree_hal_replay_recorder_reservation_proxy_vtable(proxy)->flush_range(
      proxy, offset, length);
}

static const iree_hal_buffer_vtable_t
    iree_hal_replay_recorder_reservation_buffer_vtable = {
        .destroy = iree_hal_replay_recorder_reservation_buffer_destroy,
        .export_range = iree_hal_replay_recorder_reservation_export_range,
        .map_range = iree_hal_replay_recorder_reservation_map_range,
        .unmap_range = iree_hal_replay_recorder_reservation_unmap_range,
        .invalidate_range =
            iree_hal_replay_recorder_reservation_invalidate_range,
        .flush_range = iree_hal_replay_recorder_reservation_flush_range,
};

static const iree_hal_memory_reservation_vtable_t
    iree_hal_replay_recorder_reservation_vtable = {
        .close = iree_hal_replay_recorder_reservation_close,
        .destroy = iree_hal_replay_recorder_reservation_destroy,
};

iree_status_t iree_hal_replay_recorder_create_memory_reservation(
    iree_hal_replay_recorder_t* recorder,
    iree_hal_memory_reservation_create_callback_t next,
    iree_hal_device_group_t* request_group, const iree_hal_pool_scope_t* scope,
    const iree_hal_memory_reservation_options_t* options,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t** out_reservation) {
  *out_reservation = NULL;
  iree_hal_replay_recorder_reservation_t* reservation = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      host_allocator, sizeof(*reservation), (void**)&reservation));
  iree_hal_replay_recorder_buffer_t* buffer_proxy = NULL;
  iree_status_t status = iree_hal_replay_recorder_buffer_allocate_proxy(
      host_allocator, &buffer_proxy);
  iree_hal_replay_pool_family_access_payload_t* family_payloads = NULL;
  if (iree_status_is_ok(status) && scope->family_count) {
    status = iree_allocator_malloc_array(host_allocator, scope->family_count,
                                         sizeof(*family_payloads),
                                         (void**)&family_payloads);
  }
  if (iree_status_is_ok(status)) {
    iree_hal_replay_recorder_pool_capture_families(scope, family_payloads);
  }
  iree_hal_replay_object_id_t reservation_id = IREE_HAL_REPLAY_OBJECT_ID_NONE;
  iree_hal_replay_object_id_t buffer_id = IREE_HAL_REPLAY_OBJECT_ID_NONE;
  if (iree_status_is_ok(status)) {
    status =
        iree_hal_replay_recorder_reserve_object_id(recorder, &reservation_id);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_replay_recorder_reserve_object_id(recorder, &buffer_id);
  }
  iree_hal_replay_pending_record_t pending_record = {0};
  if (iree_status_is_ok(status)) {
    status = iree_hal_replay_recorder_begin_operation(
        recorder, IREE_HAL_REPLAY_OBJECT_ID_NONE,
        IREE_HAL_REPLAY_OBJECT_ID_NONE, reservation_id,
        IREE_HAL_REPLAY_OBJECT_TYPE_NONE,
        IREE_HAL_REPLAY_OPERATION_CODE_MEMORY_RESERVATION_CREATE,
        IREE_HAL_REPLAY_PAYLOAD_TYPE_MEMORY_RESERVATION_CREATE,
        &pending_record);
  }
  iree_hal_memory_reservation_t* native_reservation = NULL;
  if (iree_status_is_ok(status)) {
    status = next.fn(next.user_data, request_group, scope, options,
                     host_allocator, &native_reservation);
  }
  iree_hal_replay_buffer_object_payload_t buffer_payload = {0};
  if (iree_status_is_ok(status)) {
    iree_hal_buffer_t* native_buffer =
        iree_hal_memory_reservation_buffer(native_reservation);
    reservation->native_reservation = native_reservation;
    reservation->buffer_proxy =
        iree_hal_replay_recorder_buffer_initialize_proxy(
            recorder, IREE_HAL_REPLAY_OBJECT_ID_NONE, buffer_id,
            IREE_HAL_REPLAY_OBJECT_ID_NONE, NULL, native_buffer, host_allocator,
            buffer_proxy);
    reservation->recorder = recorder;
    reservation->reservation_id = reservation_id;
    iree_hal_memory_reservation_initialize(
        native_buffer->memory.contract, native_buffer->memory.bindings,
        native_buffer->memory.backing,
        iree_hal_buffer_memory_type(native_buffer),
        iree_hal_buffer_byte_length(native_buffer),
        native_reservation->mapping_granularity, native_reservation->operations,
        native_reservation->source_type,
        &iree_hal_replay_recorder_reservation_vtable, host_allocator,
        &reservation->base);
    reservation->base.buffer.memory = native_buffer->memory;
    // Construction is still private: host accesses use the ordinary recorder,
    // while the reservation owner retains the common buffer layout.
    reservation->base.buffer.resource.vtable =
        (const iree_hal_resource_vtable_t*)&iree_hal_replay_recorder_reservation_buffer_vtable;
    reservation->base.projection_count = native_reservation->projection_count;
    reservation->base.projections = native_reservation->projections;
    iree_hal_replay_recorder_buffer_make_object_payload(native_buffer,
                                                        &buffer_payload);
  }
  if (pending_record.recorder) {
    const iree_hal_replay_memory_reservation_create_payload_t payload = {
        .buffer_id = buffer_id,
        .byte_length = options->byte_length,
        .min_alignment = options->min_alignment,
        .mapping_granularity = options->mapping_granularity,
        .family_count = scope->family_count,
        .host_access = scope->host.access,
        .host_mapping_modes = scope->host.modes,
        .host_cacheability = scope->host.cacheability,
    };
    const iree_const_byte_span_t operation_iovecs[] = {
        iree_make_const_byte_span(&payload, sizeof(payload)),
        iree_make_const_byte_span(
            family_payloads, scope->family_count * sizeof(*family_payloads)),
    };
    const iree_const_byte_span_t buffer_iovec =
        iree_make_const_byte_span(&buffer_payload, sizeof(buffer_payload));
    const iree_hal_replay_created_object_record_t created_objects[] = {
        {
            .object_id = reservation_id,
            .object_type = IREE_HAL_REPLAY_OBJECT_TYPE_MEMORY_RESERVATION,
            .payload_type = IREE_HAL_REPLAY_PAYLOAD_TYPE_NONE,
        },
        {
            .object_id = buffer_id,
            .object_type = IREE_HAL_REPLAY_OBJECT_TYPE_BUFFER,
            .payload_type = IREE_HAL_REPLAY_PAYLOAD_TYPE_BUFFER_OBJECT,
            .iovec_count = 1,
            .iovecs = &buffer_iovec,
        },
    };
    status = iree_hal_replay_recorder_end_creation_operation_list(
        &pending_record, status, IREE_ARRAYSIZE(operation_iovecs),
        operation_iovecs, IREE_ARRAYSIZE(created_objects), created_objects);
  }
  iree_allocator_free(host_allocator, family_payloads);
  if (iree_status_is_ok(status)) {
    *out_reservation = &reservation->base;
  } else {
    iree_hal_replay_recorder_buffer_free_proxy(host_allocator, buffer_proxy);
    iree_allocator_free(host_allocator, reservation);
  }
  return status;
}

iree_status_t iree_hal_replay_recorder_queue_map(
    iree_hal_replay_recorder_t* recorder, iree_hal_replay_object_id_t device_id,
    iree_hal_replay_object_id_t queue_id, iree_hal_queue_t* base_queue,
    iree_hal_semaphore_list_t wait_semaphore_list,
    iree_hal_semaphore_list_t signal_semaphore_list,
    iree_host_size_t mapping_count, const iree_hal_memory_mapping_t* mappings,
    iree_hal_queue_map_flags_t flags, iree_allocator_t host_allocator) {
  iree_hal_memory_mapping_t* native_mappings = NULL;
  iree_hal_replay_memory_mapping_payload_t* mapping_payloads = NULL;
  iree_status_t status = iree_ok_status();
  if (mapping_count) {
    status = iree_allocator_malloc_array(host_allocator, mapping_count,
                                         sizeof(*native_mappings),
                                         (void**)&native_mappings);
    if (iree_status_is_ok(status)) {
      status = iree_allocator_malloc_array(host_allocator, mapping_count,
                                           sizeof(*mapping_payloads),
                                           (void**)&mapping_payloads);
    }
  }
  bool can_record = iree_hal_replay_recorder_has_captured_semaphores(
                        recorder, wait_semaphore_list) &&
                    iree_hal_replay_recorder_has_captured_semaphores(
                        recorder, signal_semaphore_list);
  for (iree_host_size_t i = 0; i < mapping_count && iree_status_is_ok(status);
       ++i) {
    native_mappings[i] = mappings[i];
    iree_hal_replay_memory_mapping_payload_t* payload = &mapping_payloads[i];
    memset(payload, 0, sizeof(*payload));
    iree_hal_memory_reservation_t* target = mappings[i].reservation;
    if (target &&
        target->vtable == &iree_hal_replay_recorder_reservation_vtable) {
      iree_hal_replay_recorder_reservation_t* proxy =
          (iree_hal_replay_recorder_reservation_t*)target;
      native_mappings[i].reservation = proxy->native_reservation;
      if (proxy->recorder == recorder) {
        payload->reservation_id = proxy->reservation_id;
      }
    }
    can_record &= payload->reservation_id != IREE_HAL_REPLAY_OBJECT_ID_NONE;
    payload->scope_id = mappings[i].scope.id;
    payload->operation = mappings[i].operation;
    payload->target_offset = mappings[i].target_range.byte_offset;
    payload->byte_length = mappings[i].target_range.byte_length;
    payload->access = mappings[i].access;
    iree_hal_buffer_ref_t source_ref = {
        .buffer = mappings[i].source,
        .offset = mappings[i].source_offset,
        .length = mappings[i].target_range.byte_length,
    };
    iree_hal_replay_recorder_buffer_ref_make_payload(source_ref,
                                                     &payload->source_ref);
    if (source_ref.buffer) {
      can_record &=
          iree_hal_replay_recorder_find_buffer_id(
              recorder, source_ref.buffer) != IREE_HAL_REPLAY_OBJECT_ID_NONE;
      status = iree_hal_replay_recorder_buffer_ref_unwrap_for_call(&source_ref);
      native_mappings[i].source = source_ref.buffer;
      native_mappings[i].source_offset = source_ref.offset;
    }
  }
  iree_hal_replay_semaphore_timepoint_payload_t* wait_payloads = NULL;
  iree_host_size_t wait_payloads_size = 0;
  iree_hal_replay_semaphore_timepoint_payload_t* signal_payloads = NULL;
  iree_host_size_t signal_payloads_size = 0;
  if (iree_status_is_ok(status) && can_record) {
    status = iree_hal_replay_recorder_allocate_semaphore_payloads(
        recorder, wait_semaphore_list, host_allocator, &wait_payloads,
        &wait_payloads_size);
  }
  if (iree_status_is_ok(status) && can_record) {
    status = iree_hal_replay_recorder_allocate_semaphore_payloads(
        recorder, signal_semaphore_list, host_allocator, &signal_payloads,
        &signal_payloads_size);
  }
  iree_hal_replay_pending_record_t pending_record = {0};
  if (iree_status_is_ok(status)) {
    status = iree_hal_replay_recorder_begin_operation(
        recorder, device_id, queue_id, IREE_HAL_REPLAY_OBJECT_ID_NONE,
        IREE_HAL_REPLAY_OBJECT_TYPE_QUEUE,
        IREE_HAL_REPLAY_OPERATION_CODE_QUEUE_MAP,
        can_record ? IREE_HAL_REPLAY_PAYLOAD_TYPE_QUEUE_MAP
                   : IREE_HAL_REPLAY_PAYLOAD_TYPE_NONE,
        &pending_record);
  }
  if (pending_record.recorder && !can_record) {
    iree_hal_replay_recorder_mark_unsupported(&pending_record);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_queue_map(base_queue, wait_semaphore_list,
                                signal_semaphore_list, mapping_count,
                                native_mappings, flags);
  }
  if (pending_record.recorder) {
    const iree_hal_replay_queue_map_payload_t payload = {
        .wait_semaphore_count = wait_semaphore_list.count,
        .signal_semaphore_count = signal_semaphore_list.count,
        .mapping_count = mapping_count,
        .flags = flags,
    };
    const iree_const_byte_span_t iovecs[] = {
        iree_make_const_byte_span(&payload, sizeof(payload)),
        iree_make_const_byte_span(wait_payloads, wait_payloads_size),
        iree_make_const_byte_span(signal_payloads, signal_payloads_size),
        iree_make_const_byte_span(mapping_payloads,
                                  mapping_count * sizeof(*mapping_payloads)),
    };
    status = iree_hal_replay_recorder_end_operation_with_payload(
        &pending_record, status, can_record ? IREE_ARRAYSIZE(iovecs) : 0,
        can_record ? iovecs : NULL);
  }
  iree_allocator_free(host_allocator, signal_payloads);
  iree_allocator_free(host_allocator, wait_payloads);
  iree_allocator_free(host_allocator, mapping_payloads);
  iree_allocator_free(host_allocator, native_mappings);
  return status;
}
