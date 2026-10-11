// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/base/internal/memory.h"
#include "iree/base/internal/shm.h"
#include "iree/hal/memory/cpu_slab_provider.h"
#include "iree/hal/memory/host_memory_backend.h"

typedef struct iree_hal_host_memory_reservation_t {
  // Stable HAL view and explicit native lifecycle.
  iree_hal_memory_reservation_t base;
  // Owned process address interval, initially inaccessible.
  iree_shm_reservation_t native;
  // Sticky native replacement failure, serialized by the caller's queue edges.
  bool failed;
  // Prepared CPU execution address shared by every admitted site.
  iree_hal_buffer_native_binding_t binding;
  // Virtual geometry and borrowed progress services for child pools.
  iree_hal_buffer_backing_facts_t backing;
  // Captured process page-table identity per admitted scope.
  uint32_t projections[];
} iree_hal_host_memory_reservation_t;

static iree_status_t iree_hal_host_memory_reservation_close(
    iree_hal_memory_reservation_t* base) {
  iree_hal_host_memory_reservation_t* reservation =
      (iree_hal_host_memory_reservation_t*)base;
  return iree_shm_reservation_deinitialize(&reservation->native);
}

static void iree_hal_host_memory_reservation_destroy(
    iree_hal_memory_reservation_t* base) {
  iree_allocator_free(base->host_allocator, base);
}

static iree_status_t iree_hal_host_memory_reservation_update(
    iree_hal_memory_reservation_t* base, uint32_t projection,
    iree_host_size_t update_count,
    const iree_hal_memory_mapping_update_t* updates) {
  iree_hal_host_memory_reservation_t* reservation =
      (iree_hal_host_memory_reservation_t*)base;
  if (reservation->failed) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "reservation requires destruction after native failure");
  }
  iree_shm_mapping_update_t inline_updates[16];
  iree_shm_mapping_update_t* native_updates = inline_updates;
  if (update_count > IREE_ARRAYSIZE(inline_updates)) {
    IREE_RETURN_IF_ERROR(iree_allocator_malloc(
        base->host_allocator, update_count * sizeof(*native_updates),
        (void**)&native_updates));
  }
  for (iree_host_size_t i = 0; i < update_count; ++i) {
    iree_shm_access_t access = 0;
    if (iree_any_bit_set(updates[i].access, IREE_HAL_MEMORY_ACCESS_READ)) {
      access |= IREE_SHM_ACCESS_READ;
    }
    if (iree_any_bit_set(updates[i].access, IREE_HAL_MEMORY_ACCESS_WRITE)) {
      // CPU write mappings can also be readable. HAL permissions describe
      // valid application accesses, not a hardware isolation boundary.
      access |= IREE_SHM_ACCESS_READ | IREE_SHM_ACCESS_WRITE;
    }
    native_updates[i] = (iree_shm_mapping_update_t){
        .target_offset = (iree_host_size_t)updates[i].target_range.byte_offset,
        .byte_length = (iree_host_size_t)updates[i].target_range.byte_length,
        .source = updates[i].source.mapping_source.descriptor,
        .source_offset =
            (iree_host_size_t)updates[i].source.mapping_source.offset,
        .access = access,
    };
  }
  iree_status_t status = iree_shm_reservation_update(
      &reservation->native, update_count, native_updates);
  reservation->failed = !iree_status_is_ok(status);
  if (native_updates != inline_updates) {
    iree_allocator_free(base->host_allocator, native_updates);
  }
  return status;
}

static const iree_hal_memory_reservation_vtable_t
    iree_hal_host_memory_reservation_vtable = {
        .close = iree_hal_host_memory_reservation_close,
        .destroy = iree_hal_host_memory_reservation_destroy,
        .update = iree_hal_host_memory_reservation_update,
};

iree_status_t iree_hal_host_memory_reservation_create(
    const iree_hal_host_memory_backend_t* backend,
    iree_async_frontier_tracker_t* frontier_tracker,
    iree_hal_memory_contract_t* contract,
    const iree_hal_memory_reservation_options_t* options,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t** out_reservation) {
  if (options->byte_length > IREE_HOST_SIZE_MAX ||
      options->min_alignment > IREE_HOST_SIZE_MAX ||
      options->mapping_granularity > IREE_HOST_SIZE_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "reservation exceeds host address space");
  }
  iree_hal_host_memory_reservation_t* reservation = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      host_allocator,
      sizeof(*reservation) + contract->scope_count * sizeof(uint32_t),
      (void**)&reservation));
  const iree_host_size_t page_size = iree_memory_query_info().normal_page_size;
  iree_status_t status = iree_shm_reservation_initialize(
      (iree_host_size_t)options->byte_length,
      iree_max((iree_host_size_t)options->min_alignment, page_size),
      iree_max((iree_host_size_t)options->mapping_granularity, page_size),
      &reservation->native);
  if (iree_status_is_ok(status)) {
    reservation->binding.host_pointer = reservation->native.base;
    reservation->backing = (iree_hal_buffer_backing_facts_t){
        .notification = backend->notification,
        .tracker = frontier_tracker,
        .maintenance = backend->maintenance,
        .maintenance_range = {.byte_length = options->byte_length},
        .allocation_alignment =
            iree_max(options->min_alignment, reservation->native.granularity),
        .maintenance_alignment = 1,
    };
    iree_hal_memory_reservation_initialize(
        contract, &reservation->binding, &reservation->backing,
        IREE_HAL_CPU_SLAB_PROVIDER_MEMORY_TYPE, options->byte_length,
        reservation->native.granularity,
        &iree_hal_host_memory_reservation_vtable, host_allocator,
        &reservation->base);
    reservation->base.projection_count = 1;
    reservation->base.projections = reservation->projections;
    for (uint32_t i = 0; i < contract->scope_count; ++i) {
      reservation->projections[i] =
          contract->scopes[i].interfaces ? 0 : IREE_HAL_MEMORY_SCOPE_INVALID;
    }
    *out_reservation = &reservation->base;
  } else {
    iree_allocator_free(host_allocator, reservation);
  }
  return status;
}
