// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/memory_reservation.h"

#include <stdlib.h>
#include <string.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/device.h"
#include "libamdf/src/memory_resource.h"
#include "libamdf/src/structure.h"

amdf_status_t AMDF_CALL amdf_device_query_memory_reservation_capabilities(
    amdf_device_t* device, uint32_t address_domain_ordinal,
    amdf_memory_reservation_capabilities_t* out_capabilities) {
  if (device == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  amdf_status_t status = amdf_structure_validate_output(
      out_capabilities, AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CAPABILITIES,
      sizeof(*out_capabilities));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  if (device->vtable->query_memory_reservation_capabilities == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_memory_reservation_capabilities_t capabilities = {0};
  status = device->vtable->query_memory_reservation_capabilities(
      device, address_domain_ordinal, &capabilities);
  if (amdf_status_is_ok(status)) {
    capabilities.type = out_capabilities->type;
    capabilities.structure_size = out_capabilities->structure_size;
    capabilities.next = out_capabilities->next;
    *out_capabilities = capabilities;
  }
  return status;
}

amdf_status_t AMDF_CALL amdf_memory_reservation_create(
    amdf_instance_t* instance,
    const amdf_memory_reservation_create_info_t* create_info,
    amdf_memory_reservation_t** out_reservation) {
  if (instance == NULL || out_reservation == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  amdf_status_t status = amdf_structure_validate_input(
      create_info, AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CREATE_INFO,
      sizeof(*create_info));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  if (create_info->reserved != 0 || create_info->byte_length == 0 ||
      create_info->access_count == 0 || create_info->accesses == NULL ||
      (create_info->minimum_alignment & (create_info->minimum_alignment - 1)) !=
          0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_device_vtable_t* provider = NULL;
  for (uint32_t i = 0; i < create_info->access_count; ++i) {
    const amdf_memory_reservation_access_t* access = &create_info->accesses[i];
    if (access->device == NULL ||
        access->device->provider_instance != instance || access->access == 0) {
      return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (create_info->accesses[j].device == access->device) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
    }
    if (provider == NULL) {
      provider = access->device->vtable;
    }
    if (provider->reservation_create == NULL ||
        provider != access->device->vtable) {
      return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    }
    amdf_memory_reservation_capabilities_t capabilities = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CAPABILITIES,
        .structure_size = sizeof(capabilities),
    };
    status = amdf_device_query_memory_reservation_capabilities(
        access->device, access->address_domain_ordinal, &capabilities);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if ((access->access & ~capabilities.supported_access) != 0 ||
        (access->access & capabilities.required_access) !=
            capabilities.required_access ||
        create_info->byte_length > capabilities.maximum_byte_length ||
        create_info->byte_length % capabilities.reservation_granularity != 0 ||
        create_info->minimum_alignment > capabilities.maximum_alignment) {
      return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    }
  }
  return provider->reservation_create(instance, create_info, out_reservation);
}

amdf_status_t AMDF_CALL amdf_memory_reservation_query_access_info(
    amdf_memory_reservation_t* reservation, uint32_t access_ordinal,
    amdf_memory_reservation_access_info_t* out_info) {
  if (reservation == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_status_t status = amdf_structure_validate_output(
      out_info, AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO,
      sizeof(*out_info));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  if (access_ordinal >= reservation->access_count) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  amdf_memory_reservation_access_info_t info =
      reservation->accesses[access_ordinal].info;
  info.type = out_info->type;
  info.structure_size = out_info->structure_size;
  info.next = out_info->next;
  *out_info = info;
  return AMDF_STATUS_OK;
}

static int amdf_memory_mapping_compare(const void* a, const void* b) {
  const uint64_t lhs =
      ((const amdf_memory_mapping_update_t*)a)->target_byte_offset;
  const uint64_t rhs =
      ((const amdf_memory_mapping_update_t*)b)->target_byte_offset;
  return (lhs > rhs) - (lhs < rhs);
}

// This is the public range/ownership boundary. Native providers consume these
// established facts directly and add only their concrete native source checks.
static amdf_status_t amdf_memory_reservation_validate_update(
    const amdf_memory_reservation_access_state_t* target,
    const amdf_memory_mapping_update_t* update) {
  const uint64_t granularity = target->info.mapping_granularity;
  if (update->byte_length == 0 ||
      update->target_byte_offset > target->info.byte_length ||
      update->byte_length >
          target->info.byte_length - update->target_byte_offset ||
      update->target_byte_offset % granularity != 0 ||
      update->byte_length % granularity != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  if (update->memory == NULL) {
    return update->memory_access_ordinal == 0 && update->access == 0 &&
                   update->source_byte_offset == 0
               ? AMDF_STATUS_OK
               : amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_memory_t* memory = update->memory;
  if (update->memory_access_ordinal >= memory->info.access_count ||
      update->source_byte_offset > memory->info.byte_length ||
      update->byte_length >
          memory->info.byte_length - update->source_byte_offset) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  const amdf_memory_access_state_t* source =
      &memory->accesses[update->memory_access_ordinal];
  if (source->device != target->device ||
      source->info.address_domain_ordinal !=
          target->info.address_domain_ordinal ||
      (source->info.flags & AMDF_MEMORY_FLAG_MAPPING_SOURCE) == 0 ||
      update->access == 0 || (update->access & ~source->info.access) != 0 ||
      (update->access & ~target->info.access) != 0 ||
      (update->access & target->required_access) != target->required_access ||
      (memory->info.source_byte_offset + update->source_byte_offset) %
              granularity !=
          0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  return AMDF_STATUS_OK;
}

amdf_status_t AMDF_CALL amdf_memory_reservation_update(
    amdf_memory_reservation_t* reservation, uint32_t access_ordinal,
    uint32_t update_count, const amdf_memory_mapping_update_t* updates) {
  if (reservation == NULL || (update_count != 0 && updates == NULL)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (access_ordinal >= reservation->access_count) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  amdf_memory_reservation_access_state_t* target =
      &reservation->accesses[access_ordinal];
  amdf_status_t* failure =
      &reservation->accesses[target->info.mapping_access_ordinal].failure;
  if (!amdf_status_is_ok(*failure)) {
    return *failure;
  }
  if (update_count == 0) {
    return AMDF_STATUS_OK;
  }
  for (uint32_t i = 0; i < update_count; ++i) {
    const amdf_status_t status =
        amdf_memory_reservation_validate_update(target, &updates[i]);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
  }
  amdf_memory_mapping_update_t* ordered = NULL;
  amdf_status_t status = amdf_calloc_array(
      reservation->host_allocator, update_count, sizeof(*ordered),
      amdf_alignof(amdf_memory_mapping_update_t), (void**)&ordered);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  memcpy(ordered, updates, update_count * sizeof(*ordered));
  qsort(ordered, update_count, sizeof(*ordered), amdf_memory_mapping_compare);
  for (uint32_t i = 1; i < update_count && amdf_status_is_ok(status); ++i) {
    if (ordered[i].target_byte_offset <
        ordered[i - 1].target_byte_offset + ordered[i - 1].byte_length) {
      status = amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
    }
  }
  bool mutation_started = false;
  if (amdf_status_is_ok(status)) {
    status = reservation->vtable->update(
        reservation, access_ordinal, update_count, ordered, &mutation_started);
  }
  if (!amdf_status_is_ok(status) && mutation_started) {
    *failure = status;
  }
  amdf_free(reservation->host_allocator, ordered);
  return status;
}

amdf_status_t AMDF_CALL
amdf_memory_reservation_destroy(amdf_memory_reservation_t* reservation) {
  if (reservation == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return reservation->vtable->destroy(reservation);
}
