// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/memory_reservation.h"

#include "libamdf/src/allocator.h"
#include "libamdf/src/gpu/device.h"
#include "libamdf/src/gpu/umd/memory_reservation.h"
#include "libamdf/src/instance.h"
#include "libamdf/src/memory_resource.h"

typedef struct amdf_gpu_memory_reservation_t {
  // Provider-neutral ownership and cached consumer metadata.
  amdf_memory_reservation_t base;
  // Complete native reservation group owned through terminal release.
  amdf_gpu_umd_memory_reservation_t* umd;
} amdf_gpu_memory_reservation_t;

amdf_status_t amdf_gpu_device_query_memory_reservation_capabilities(
    amdf_device_t* device, uint32_t address_domain_ordinal,
    amdf_memory_reservation_capabilities_t* out_capabilities) {
  return amdf_gpu_umd_query_memory_reservation_capabilities(
      amdf_gpu_device_get_umd(device), address_domain_ordinal,
      out_capabilities);
}

static amdf_status_t amdf_gpu_memory_reservation_apply(
    amdf_memory_reservation_t* base, uint32_t access_ordinal,
    uint32_t update_count, const amdf_memory_mapping_update_t* updates,
    bool* mutation_started, amdf_gpu_umd_kernel_queue_t* queue,
    uint64_t submission, bool* accepted) {
  amdf_gpu_memory_reservation_t* reservation =
      (amdf_gpu_memory_reservation_t*)base;
  amdf_gpu_umd_mapping_update_t* native = NULL;
  amdf_status_t status = amdf_calloc_array(
      base->host_allocator, update_count, sizeof(*native),
      amdf_alignof(amdf_gpu_umd_mapping_update_t), (void**)&native);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  for (uint32_t i = 0; i < update_count; ++i) {
    native[i].target_byte_offset = updates[i].target_byte_offset;
    native[i].byte_length = updates[i].byte_length;
    native[i].access = updates[i].access;
    if (updates[i].memory != NULL) {
      const amdf_memory_t* memory = updates[i].memory;
      const uint32_t owner = memory->accesses[updates[i].memory_access_ordinal]
                                 .native_owner_ordinal;
      native[i].memory = memory->accesses[owner].native;
      native[i].source_byte_offset =
          memory->info.source_byte_offset + updates[i].source_byte_offset;
    }
  }
  status = queue != NULL
               ? amdf_gpu_umd_memory_reservation_update_on_queue(
                     reservation->umd, queue, access_ordinal, update_count,
                     native, submission, mutation_started, accepted)
               : amdf_gpu_umd_memory_reservation_update(
                     reservation->umd, access_ordinal, update_count, native,
                     mutation_started);
  amdf_free(base->host_allocator, native);
  return status;
}

static amdf_status_t amdf_gpu_memory_reservation_update(
    amdf_memory_reservation_t* base, uint32_t access_ordinal,
    uint32_t update_count, const amdf_memory_mapping_update_t* updates,
    bool* mutation_started) {
  return amdf_gpu_memory_reservation_apply(base, access_ordinal, update_count,
                                           updates, mutation_started, NULL, 0,
                                           NULL);
}

amdf_status_t amdf_gpu_memory_reservation_update_on_queue(
    amdf_memory_reservation_t* base, amdf_gpu_umd_kernel_queue_t* queue,
    uint32_t access_ordinal, uint32_t update_count,
    const amdf_memory_mapping_update_t* updates, uint64_t submission,
    bool* mutation_started, bool* accepted) {
  return amdf_gpu_memory_reservation_apply(base, access_ordinal, update_count,
                                           updates, mutation_started, queue,
                                           submission, accepted);
}

static amdf_status_t amdf_gpu_memory_reservation_detach(
    amdf_memory_reservation_t* base, uint32_t access_ordinal) {
  amdf_gpu_memory_reservation_t* reservation =
      (amdf_gpu_memory_reservation_t*)base;
  return amdf_gpu_umd_memory_reservation_detach(reservation->umd,
                                                access_ordinal);
}

static amdf_status_t amdf_gpu_memory_reservation_destroy(
    amdf_memory_reservation_t* base) {
  amdf_gpu_memory_reservation_t* reservation =
      (amdf_gpu_memory_reservation_t*)base;
  const amdf_status_t status =
      amdf_gpu_umd_memory_reservation_destroy(reservation->umd);
  amdf_free(base->host_allocator, reservation);
  return status;
}

static const amdf_memory_reservation_vtable_t
    amdf_gpu_memory_reservation_vtable = {
        .update = amdf_gpu_memory_reservation_update,
        .detach = amdf_gpu_memory_reservation_detach,
        .destroy = amdf_gpu_memory_reservation_destroy,
};

amdf_status_t amdf_gpu_memory_reservation_create(
    amdf_instance_t* instance,
    const amdf_memory_reservation_create_info_t* create_info,
    amdf_memory_reservation_t** out_reservation) {
  const amdf_allocator_t allocator = amdf_instance_host_allocator(instance);
  const uint32_t count = create_info->access_count;
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t j = 0; j < i; ++j) {
      if (create_info->accesses[i].access != create_info->accesses[j].access &&
          amdf_gpu_umd_devices_share_mapping_domain(
              amdf_gpu_device_get_umd(create_info->accesses[i].device),
              amdf_gpu_device_get_umd(create_info->accesses[j].device))) {
        return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
      }
    }
  }
  amdf_gpu_memory_reservation_t* reservation = NULL;
  amdf_status_t status = amdf_calloc_with_trailing(
      allocator, sizeof(*reservation),
      count * sizeof(amdf_memory_reservation_access_state_t),
      amdf_alignof(amdf_gpu_memory_reservation_t), (void**)&reservation);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  reservation->base.host_allocator = allocator;
  reservation->base.vtable = &amdf_gpu_memory_reservation_vtable;
  reservation->base.access_count = count;
  reservation->base.accesses =
      (amdf_memory_reservation_access_state_t*)(reservation + 1);
  amdf_gpu_umd_device_t** devices = NULL;
  status =
      amdf_calloc_array(allocator, count, sizeof(*devices),
                        amdf_alignof(amdf_gpu_umd_device_t*), (void**)&devices);
  if (amdf_status_is_ok(status)) {
    for (uint32_t i = 0; i < count; ++i) {
      devices[i] = amdf_gpu_device_get_umd(create_info->accesses[i].device);
    }
    status = amdf_gpu_umd_memory_reservation_create(
        allocator, count, devices, create_info->byte_length,
        create_info->minimum_alignment, &reservation->umd);
  }
  amdf_free(allocator, devices);
  if (amdf_status_is_ok(status)) {
    for (uint32_t i = 0; i < count; ++i) {
      amdf_memory_reservation_access_state_t* access =
          &reservation->base.accesses[i];
      access->device = create_info->accesses[i].device;
      access->info = (amdf_memory_reservation_access_info_t){
          .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO,
          .structure_size = sizeof(access->info),
          .device_id = amdf_gpu_device_get_info(access->device)->id,
          .ordinal = i,
          .address_domain_ordinal =
              create_info->accesses[i].address_domain_ordinal,
          .access = create_info->accesses[i].access,
          .byte_length = create_info->byte_length,
          .reset_epoch = amdf_gpu_device_query_reset_epoch(access->device),
      };
      amdf_gpu_umd_memory_reservation_query_access(
          reservation->umd, i, &access->info.address,
          &access->info.mapping_access_ordinal,
          &access->info.mapping_granularity, &access->required_access);
    }
    *out_reservation = &reservation->base;
  } else {
    amdf_free(allocator, reservation);
  }
  return status;
}
