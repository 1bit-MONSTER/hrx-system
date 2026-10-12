// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory_reservation.h"

#include <stdlib.h>

#include "iree/hal/device_group.h"
#include "iree/hal/memory_backend.h"
#include "iree/hal/queue.h"

IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_query_capabilities(
    iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_capabilities_t* out_capabilities) {
  memset(out_capabilities, 0, sizeof(*out_capabilities));
  if (!group) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "reservation group is null");
  }
  IREE_RETURN_IF_ERROR(iree_hal_pool_scope_validate(group, scope));
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < iree_hal_device_group_device_count(group) &&
       iree_status_is_ok(status) && !out_capabilities->max_byte_length;
       ++i) {
    const iree_hal_memory_backend_t* backend = iree_hal_device_memory_backend(
        iree_hal_device_group_device_at(group, i));
    if (backend && backend->query_reservation) {
      status = backend->query_reservation(backend, group, scope, host_allocator,
                                          out_capabilities);
    }
  }
  if (!iree_status_is_ok(status)) {
    memset(out_capabilities, 0, sizeof(*out_capabilities));
  }
  return status;
}

IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_create(
    iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    const iree_hal_memory_reservation_options_t* options,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t** out_reservation) {
  *out_reservation = NULL;
  if (!group || !options || !options->byte_length ||
      !iree_device_size_is_valid_alignment(options->min_alignment) ||
      !iree_device_size_is_valid_alignment(options->mapping_granularity)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid reservation group or geometry");
  }
  IREE_RETURN_IF_ERROR(iree_hal_pool_scope_validate(group, scope));
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < iree_hal_device_group_device_count(group) &&
                               iree_status_is_ok(status) && !*out_reservation;
       ++i) {
    const iree_hal_memory_backend_t* backend = iree_hal_device_memory_backend(
        iree_hal_device_group_device_at(group, i));
    if (backend && backend->reserve) {
      status = backend->reserve(backend, group, scope, options, host_allocator,
                                out_reservation);
    }
  }
  if (iree_status_is_ok(status) && !*out_reservation) {
    status =
        iree_make_status(IREE_STATUS_UNAVAILABLE,
                         "no memory backend supports this reservation scope");
  }
  return status;
}

IREE_API_EXPORT iree_hal_buffer_t* iree_hal_memory_reservation_buffer(
    iree_hal_memory_reservation_t* reservation) {
  return &reservation->buffer;
}

IREE_API_EXPORT iree_device_size_t
iree_hal_memory_reservation_mapping_granularity(
    const iree_hal_memory_reservation_t* reservation) {
  return reservation->mapping_granularity;
}

IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_resolve_projection(
    const iree_hal_memory_reservation_t* reservation,
    iree_hal_memory_scope_t scope, uint32_t* out_projection) {
  *out_projection = IREE_HAL_MEMORY_SCOPE_INVALID;
  const iree_hal_memory_contract_t* contract =
      reservation->buffer.memory.contract;
  if (scope.domain != contract->domain || !scope.id ||
      scope.id >= contract->scope_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "scope is not an exact reservation site");
  }
  if (reservation->projections[scope.id] == IREE_HAL_MEMORY_SCOPE_INVALID) {
    return iree_make_status(IREE_STATUS_PERMISSION_DENIED,
                            "scope is not admitted by the reservation");
  }
  *out_projection = reservation->projections[scope.id];
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_destroy(
    iree_hal_memory_reservation_t* reservation) {
  if (!reservation) {
    return iree_ok_status();
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_status_t status = reservation->vtable->close(reservation);
  reservation->closed = true;
  iree_hal_buffer_release(&reservation->buffer);
  IREE_TRACE_ZONE_END(z0);
  return status;
}

static void iree_hal_memory_reservation_buffer_destroy(
    iree_hal_buffer_t* buffer) {
  iree_hal_memory_reservation_t* reservation =
      (iree_hal_memory_reservation_t*)buffer;
  // The explicit address owner must be consumed through checked destruction;
  // ordinary buffer references retain metadata only.
  IREE_ASSERT(reservation->closed);
  iree_hal_memory_contract_release(buffer->memory.contract);
  reservation->vtable->destroy(reservation);
}

static iree_status_t iree_hal_memory_reservation_export(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length,
    iree_hal_external_storage_class_t requested_class,
    iree_hal_external_storage_t* out_storage,
    iree_hal_buffer_range_t* out_logical_range) {
  return iree_make_status(
      IREE_STATUS_UNAVAILABLE,
      "virtual reservations have no external storage payload");
}

static iree_status_t iree_hal_memory_reservation_map_range(
    iree_hal_buffer_t* buffer, iree_hal_mapping_mode_t mode,
    iree_hal_memory_access_t access, iree_hal_buffer_map_flags_t flags,
    iree_device_size_t offset, iree_device_size_t length,
    iree_hal_buffer_mapping_t* mapping) {
  return iree_hal_buffer_native_host_span(buffer, offset, length,
                                          &mapping->contents);
}

static iree_status_t iree_hal_memory_reservation_unmap_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length, iree_hal_buffer_mapping_t* mapping) {
  return iree_ok_status();
}

static iree_status_t iree_hal_memory_reservation_invalidate_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  iree_atomic_thread_fence(iree_memory_order_acquire);
  return iree_ok_status();
}

static iree_status_t iree_hal_memory_reservation_flush_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  iree_atomic_thread_fence(iree_memory_order_release);
  return iree_ok_status();
}

static const iree_hal_buffer_vtable_t
    iree_hal_memory_reservation_buffer_vtable = {
        .destroy = iree_hal_memory_reservation_buffer_destroy,
        .export_range = iree_hal_memory_reservation_export,
        .map_range = iree_hal_memory_reservation_map_range,
        .unmap_range = iree_hal_memory_reservation_unmap_range,
        .invalidate_range = iree_hal_memory_reservation_invalidate_range,
        .flush_range = iree_hal_memory_reservation_flush_range,
};

IREE_API_EXPORT void iree_hal_memory_reservation_initialize(
    iree_hal_memory_contract_t* contract,
    const iree_hal_buffer_native_binding_t* bindings,
    const iree_hal_buffer_backing_facts_t* backing,
    iree_hal_memory_type_t memory_type, iree_device_size_t byte_length,
    iree_device_size_t mapping_granularity,
    iree_hal_memory_mapping_operations_t operations,
    iree_hal_memory_backend_type_t source_type,
    const iree_hal_memory_reservation_vtable_t* vtable,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t* reservation) {
  iree_hal_buffer_initialize(
      (iree_hal_buffer_placement_t){0}, &reservation->buffer, byte_length, 0,
      byte_length, memory_type, contract->allowed_access,
      contract->allowed_usage, &iree_hal_memory_reservation_buffer_vtable,
      &reservation->buffer);
  reservation->buffer.memory.contract = contract;
  iree_hal_memory_contract_retain(contract);
  reservation->buffer.memory.bindings = bindings;
  reservation->buffer.memory.backing = backing;
  reservation->buffer.host_binding_index =
      contract->binding_layout.host_binding_index;
  reservation->vtable = vtable;
  reservation->host_allocator = host_allocator;
  reservation->mapping_granularity = mapping_granularity;
  reservation->operations = operations;
  reservation->source_type = source_type;
}

//===----------------------------------------------------------------------===//
// Captured host mapping transaction
//===----------------------------------------------------------------------===//

typedef struct iree_hal_memory_mapping_capture_t {
  // Target owner, retained through its stable buffer when requested.
  iree_hal_memory_reservation_t* reservation;
  // Exact native page-table projection within the target.
  uint32_t projection;
  // Source wrapper, resolved only after allocation dependencies complete.
  iree_hal_buffer_t* source;
  // Prepared source identity slot in the source contract.
  iree_hal_buffer_native_binding_slot_t source_slot;
  // Source logical view offset.
  iree_device_size_t source_offset;
  // Validated target geometry and access; native source filled at execution.
  iree_hal_memory_mapping_update_t update;
} iree_hal_memory_mapping_capture_t;

typedef struct iree_hal_memory_mapping_batch_t {
  // Captured host-call owner, also released if the submission is cancelled.
  iree_hal_resource_t resource;
  // Allocator for the single packet containing all arrays.
  iree_allocator_t host_allocator;
  // Buffer wrapper retention policy.
  iree_hal_queue_map_flags_t flags;
  // Number of initialized captures, including on capture failure.
  iree_host_size_t count;
  // Contiguous normalized native update scratch after the capture array.
  iree_hal_memory_mapping_update_t* updates;
  // Captures sorted by target owner, projection and offset before submission.
  iree_hal_memory_mapping_capture_t captures[];
} iree_hal_memory_mapping_batch_t;

static void iree_hal_memory_mapping_batch_destroy(
    iree_hal_resource_t* resource) {
  iree_hal_memory_mapping_batch_t* batch =
      (iree_hal_memory_mapping_batch_t*)resource;
  if (!iree_any_bit_set(batch->flags, IREE_HAL_QUEUE_MAP_FLAG_BORROW_BUFFERS)) {
    for (iree_host_size_t i = 0; i < batch->count; ++i) {
      iree_hal_buffer_release(batch->captures[i].source);
      iree_hal_buffer_release(&batch->captures[i].reservation->buffer);
    }
  }
  iree_allocator_free(batch->host_allocator, batch);
}

static const iree_hal_resource_vtable_t iree_hal_memory_mapping_batch_vtable = {
    .destroy = iree_hal_memory_mapping_batch_destroy,
};

static iree_status_t iree_hal_memory_mapping_validate(
    const iree_hal_memory_mapping_t* mapping, const void* domain) {
  if (!mapping->reservation) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "mapping target is null");
  }
  const iree_hal_memory_reservation_t* reservation = mapping->reservation;
  const iree_hal_memory_contract_t* contract =
      reservation->buffer.memory.contract;
  if ((unsigned)mapping->operation >
          IREE_HAL_MEMORY_MAPPING_OPERATION_PROTECT ||
      (mapping->operation == IREE_HAL_MEMORY_MAPPING_OPERATION_UNBIND &&
       mapping->source) ||
      ((mapping->operation == IREE_HAL_MEMORY_MAPPING_OPERATION_BIND ||
        mapping->operation == IREE_HAL_MEMORY_MAPPING_OPERATION_PROTECT) &&
       !mapping->source)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "mapping operation has an invalid source");
  }
  if (!(reservation->operations & (1u << mapping->operation))) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "reservation does not support mapping operation %u",
                            (unsigned)mapping->operation);
  }
  if (contract->domain != domain || mapping->scope.domain != domain ||
      mapping->scope.id >= contract->scope_count) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "mapping scopes and queue must belong to one group");
  }
  const iree_hal_buffer_range_t range = mapping->target_range;
  const iree_device_size_t granularity = reservation->mapping_granularity;
  if (!range.byte_length ||
      range.byte_offset > reservation->buffer.byte_length ||
      range.byte_length > reservation->buffer.byte_length - range.byte_offset ||
      !iree_device_size_has_alignment(range.byte_offset, granularity) ||
      !iree_device_size_has_alignment(range.byte_length, granularity)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "mapping target range is outside or misaligned");
  }
  if (!mapping->source) {
    if (mapping->source_offset || mapping->access) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "detach requires zero source offset and access");
    }
  } else {
    if (iree_any_bit_set(mapping->access, ~IREE_HAL_MEMORY_ACCESS_ALL) ||
        !iree_all_bits_set(mapping->source->allowed_access, mapping->access) ||
        !iree_all_bits_set(reservation->buffer.allowed_access,
                           mapping->access)) {
      return iree_make_status(
          IREE_STATUS_PERMISSION_DENIED,
          "mapping access exceeds source or target permissions");
    }
    if (mapping->source_offset > mapping->source->byte_length ||
        range.byte_length >
            mapping->source->byte_length - mapping->source_offset) {
      return iree_make_status(
          IREE_STATUS_OUT_OF_RANGE,
          "mapping source range is outside its logical buffer");
    }
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_memory_mapping_capture(
    const iree_hal_memory_mapping_t* mapping,
    iree_hal_memory_mapping_batch_t* batch) {
  iree_hal_memory_reservation_t* reservation = mapping->reservation;
  const iree_hal_memory_contract_t* target_contract =
      reservation->buffer.memory.contract;
  const iree_hal_memory_contract_t* source_contract =
      mapping->source ? mapping->source->memory.contract : NULL;
  if (mapping->source && (!source_contract ||
                          source_contract->domain != target_contract->domain)) {
    return iree_make_status(IREE_STATUS_PERMISSION_DENIED,
                            "source has no mapping contract in this group");
  }
  if (source_contract &&
      source_contract->mapping_source.requirements.minimum_granularity &&
      source_contract->mapping_source.type != reservation->source_type) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "source and reservation use different native "
                            "mapping formats");
  }
  const uint32_t begin = mapping->scope.id ? mapping->scope.id : 1;
  const uint32_t end =
      mapping->scope.id ? begin + 1 : target_contract->scope_count;
  const iree_host_size_t first_capture = batch->count;
  iree_status_t status = iree_ok_status();
  for (uint32_t id = begin; id < end && iree_status_is_ok(status); ++id) {
    const uint32_t projection = reservation->projections[id];
    if (projection == IREE_HAL_MEMORY_SCOPE_INVALID) {
      continue;
    }
    uint16_t slot = IREE_HAL_BUFFER_NATIVE_BINDING_INDEX_NONE;
    if (source_contract) {
      slot = source_contract->scopes[id]
                 .bindings[IREE_HAL_BUFFER_INTERFACE_MAPPING_SOURCE];
      if (slot == IREE_HAL_BUFFER_NATIVE_BINDING_INDEX_NONE) {
        status =
            iree_make_status(IREE_STATUS_PERMISSION_DENIED,
                             "source is not remappable at target scope %u", id);
        continue;
      }
    }
    bool captured = false;
    for (iree_host_size_t j = first_capture; j < batch->count; ++j) {
      captured |= batch->captures[j].projection == projection;
    }
    if (captured) {
      continue;
    }
    iree_hal_memory_mapping_capture_t* capture =
        &batch->captures[batch->count++];
    *capture = (iree_hal_memory_mapping_capture_t){
        .reservation = reservation,
        .projection = projection,
        .source = mapping->source,
        .source_slot = {.index = slot,
                        .type = IREE_HAL_BUFFER_INTERFACE_MAPPING_SOURCE},
        .source_offset = mapping->source_offset,
        .update = {.scope_id = id,
                   .target_range = mapping->target_range,
                   .access = mapping->access,
                   .operation = mapping->operation},
    };
    if (!iree_any_bit_set(batch->flags,
                          IREE_HAL_QUEUE_MAP_FLAG_BORROW_BUFFERS)) {
      iree_hal_buffer_retain(capture->source);
      iree_hal_buffer_retain(&reservation->buffer);
    }
  }
  if (iree_status_is_ok(status) && first_capture == batch->count) {
    status = iree_make_status(IREE_STATUS_PERMISSION_DENIED,
                              "mapping target scope is excluded");
  }
  return status;
}

static int iree_hal_memory_mapping_compare(const void* lhs, const void* rhs) {
  const iree_hal_memory_mapping_capture_t* a = lhs;
  const iree_hal_memory_mapping_capture_t* b = rhs;
  if (a->reservation != b->reservation) {
    return (uintptr_t)a->reservation < (uintptr_t)b->reservation ? -1 : 1;
  }
  if (a->projection != b->projection) {
    return a->projection < b->projection ? -1 : 1;
  }
  return a->update.target_range.byte_offset < b->update.target_range.byte_offset
             ? -1
         : a->update.target_range.byte_offset >
                 b->update.target_range.byte_offset
             ? 1
             : 0;
}

static iree_status_t iree_hal_memory_mapping_batch_execute(
    void* user_data, const uint64_t args[4],
    iree_hal_host_call_context_t* context) {
  iree_hal_memory_mapping_batch_t* batch = user_data;
  if (!batch) {
    return iree_ok_status();
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_status_t status = iree_ok_status();
  // Resolve every source after prerequisite allocations, before mutating any
  // destination. No native source is recovered by walking back to its pool.
  for (iree_host_size_t i = 0; i < batch->count && iree_status_is_ok(status);
       ++i) {
    const iree_hal_memory_mapping_capture_t* capture = &batch->captures[i];
    batch->updates[i] = capture->update;
    if (capture->source) {
      iree_hal_buffer_memory_view_t memory =
          iree_hal_buffer_memory_view(capture->source);
      if (!memory.bindings) {
        status =
            iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                             "mapping source allocation has not committed");
      } else {
        iree_hal_buffer_native_binding_t source =
            iree_hal_buffer_memory_native_binding(&memory,
                                                  capture->source_slot);
        if (!source.mapping_source.descriptor ||
            source.mapping_source.offset >
                UINT64_MAX - capture->source_offset ||
            !iree_device_size_has_alignment(
                source.mapping_source.offset + capture->source_offset,
                source.mapping_source.granularity) ||
            !iree_device_size_has_alignment(
                capture->update.target_range.byte_length,
                source.mapping_source.granularity)) {
          status = iree_make_status(
              IREE_STATUS_FAILED_PRECONDITION,
              "native mapping source is unavailable or misaligned");
        } else {
          source.mapping_source.offset += capture->source_offset;
          batch->updates[i].source = source;
        }
      }
    }
  }
  for (iree_host_size_t i = 0; i < batch->count && iree_status_is_ok(status);) {
    iree_hal_memory_mapping_capture_t* first = &batch->captures[i];
    iree_host_size_t end = i + 1;
    while (end < batch->count &&
           batch->captures[end].reservation == first->reservation &&
           batch->captures[end].projection == first->projection) {
      ++end;
    }
    status = first->reservation->vtable->update(
        first->reservation, first->projection, end - i, &batch->updates[i]);
    i = end;
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_prepare_map(
    iree_hal_queue_t* queue, iree_host_size_t mapping_count,
    const iree_hal_memory_mapping_t* mappings, iree_hal_queue_map_flags_t flags,
    iree_hal_host_call_t* out_call) {
  *out_call = (iree_hal_host_call_t){0};
  if (!mapping_count) {
    *out_call =
        iree_hal_make_host_call(iree_hal_memory_mapping_batch_execute, NULL);
    return iree_ok_status();
  }
  const void* domain = iree_hal_queue_family(queue)->memory.domain;
  iree_host_size_t capacity = 0;
  for (iree_host_size_t i = 0; i < mapping_count; ++i) {
    IREE_RETURN_IF_ERROR(
        iree_hal_memory_mapping_validate(&mappings[i], domain));
    if (!iree_host_size_checked_add(
            capacity, mappings[i].reservation->projection_count, &capacity)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "mapping count overflows");
    }
  }
  const iree_host_size_t entry_size =
      sizeof(iree_hal_memory_mapping_capture_t) +
      sizeof(iree_hal_memory_mapping_update_t);
  if (capacity >
      (IREE_HOST_SIZE_MAX - sizeof(iree_hal_memory_mapping_batch_t)) /
          entry_size) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "mapping packet size overflows");
  }
  iree_allocator_t host_allocator = mappings[0].reservation->host_allocator;
  iree_hal_memory_mapping_batch_t* batch = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      host_allocator, sizeof(*batch) + capacity * entry_size, (void**)&batch));
  iree_hal_resource_initialize(&iree_hal_memory_mapping_batch_vtable,
                               &batch->resource);
  batch->host_allocator = host_allocator;
  batch->flags = flags;
  batch->updates =
      (iree_hal_memory_mapping_update_t*)(batch->captures + capacity);
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < mapping_count && iree_status_is_ok(status);
       ++i) {
    status = iree_hal_memory_mapping_capture(&mappings[i], batch);
  }
  if (iree_status_is_ok(status)) {
    qsort(batch->captures, batch->count, sizeof(batch->captures[0]),
          iree_hal_memory_mapping_compare);
    for (iree_host_size_t i = 1; i < batch->count && iree_status_is_ok(status);
         ++i) {
      const iree_hal_memory_mapping_capture_t* previous =
          &batch->captures[i - 1];
      const iree_hal_memory_mapping_capture_t* current = &batch->captures[i];
      if (previous->reservation == current->reservation &&
          previous->projection == current->projection &&
          previous->update.target_range.byte_offset +
                  previous->update.target_range.byte_length >
              current->update.target_range.byte_offset) {
        status =
            iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                             "mapping batch overlaps in one native projection");
      }
    }
  }
  if (iree_status_is_ok(status)) {
    *out_call = iree_hal_make_host_call_with_resource(
        iree_hal_memory_mapping_batch_execute, batch, &batch->resource);
  } else {
    iree_hal_resource_release(batch);
  }
  return status;
}

IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_queue_map(
    iree_hal_queue_t* queue, iree_hal_semaphore_list_t waits,
    iree_hal_semaphore_list_t signals, iree_host_size_t mapping_count,
    const iree_hal_memory_mapping_t* mappings,
    iree_hal_queue_map_flags_t flags) {
  iree_hal_host_call_t call;
  IREE_RETURN_IF_ERROR(iree_hal_memory_reservation_prepare_map(
      queue, mapping_count, mappings, flags, &call));
  const uint64_t args[4] = {0};
  iree_status_t status =
      iree_hal_queue_host_call(queue, waits, signals, call, args, 0);
  iree_hal_resource_release(call.resource);
  return status;
}
