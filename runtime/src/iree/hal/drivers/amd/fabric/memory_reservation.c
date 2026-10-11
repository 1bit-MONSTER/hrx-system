// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/device_group.h"
#include "iree/hal/drivers/amd/fabric/memory.h"
#include "iree/hal/drivers/amd/fabric/memory_backend.h"
#include "iree/hal/drivers/amd/status.h"

// A native consumer and its canonical shared page-table projection. Distinct
// consumers may alias one projection but still require exact source accesses.
typedef struct iree_hal_amd_fabric_reservation_access_t {
  // Consumer selected by the admitted families.
  amdf_memory_reservation_access_t native;
  // Cached live limits, including native permissions that cannot be narrowed.
  amdf_memory_reservation_capabilities_t capabilities;
  // Stable projection facts queried once after native construction.
  amdf_memory_reservation_access_info_t info;
  // Dense HAL projection identity, shared by native aliases.
  uint32_t projection;
} iree_hal_amd_fabric_reservation_access_t;

typedef struct iree_hal_amd_fabric_memory_reservation_t {
  // Stable HAL execution view and explicit address ownership.
  iree_hal_memory_reservation_t base;
  // Borrowed group-lifetime core API.
  const amdf_api_t* api;
  // Owned native reservation, consumed by checked close.
  amdf_memory_reservation_t* native;
  // Number of exact native consumers.
  uint32_t access_count;
  // Consumer facts in native access-ordinal order.
  iree_hal_amd_fabric_reservation_access_t* accesses;
  // Native access ordinal per exact HAL site, INVALID for excluded sites.
  uint32_t* scope_accesses;
  // Dense shared page-table projection per exact HAL site.
  uint32_t* projections;
  // One ordinary GPU execution address per native consumer.
  iree_hal_buffer_native_binding_t* bindings;
  // Virtual geometry and borrowed progress services for child pools.
  iree_hal_buffer_backing_facts_t backing;
} iree_hal_amd_fabric_memory_reservation_t;

static iree_status_t iree_hal_amd_fabric_memory_reservation_close(
    iree_hal_memory_reservation_t* base) {
  iree_hal_amd_fabric_memory_reservation_t* reservation =
      (iree_hal_amd_fabric_memory_reservation_t*)base;
  const amdf_status_t status =
      reservation->api->memory_reservation_destroy(reservation->native);
  reservation->native = NULL;
  return IREE_HAL_AMD_STATUS_FROM_AMDF(status, "memory_reservation_destroy");
}

static void iree_hal_amd_fabric_memory_reservation_destroy(
    iree_hal_memory_reservation_t* base) {
  iree_allocator_free(base->host_allocator, base);
}

static iree_status_t iree_hal_amd_fabric_memory_reservation_update(
    iree_hal_memory_reservation_t* base, uint32_t projection,
    iree_host_size_t update_count,
    const iree_hal_memory_mapping_update_t* updates) {
  iree_hal_amd_fabric_memory_reservation_t* reservation =
      (iree_hal_amd_fabric_memory_reservation_t*)base;
  if (update_count > UINT32_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "mapping vector exceeds native count capacity");
  }
  amdf_memory_mapping_update_t inline_updates[16];
  amdf_memory_mapping_update_t* native_updates = inline_updates;
  if (update_count > IREE_ARRAYSIZE(inline_updates)) {
    IREE_RETURN_IF_ERROR(iree_allocator_malloc_array_uninitialized(
        base->host_allocator, update_count, sizeof(*native_updates),
        (void**)&native_updates));
  }
  iree_status_t status = iree_ok_status();
  for (uint32_t a = 0;
       a < reservation->access_count && iree_status_is_ok(status); ++a) {
    const iree_hal_amd_fabric_reservation_access_t* access =
        &reservation->accesses[a];
    if (access->projection != projection) {
      continue;
    }
    uint32_t count = 0;
    for (iree_host_size_t i = 0; i < update_count; ++i) {
      if (reservation->scope_accesses[updates[i].scope_id] != a) {
        continue;
      }
      const iree_hal_amd_fabric_mapping_source_t* source =
          updates[i].source.mapping_source.descriptor;
      amdf_memory_access_t permissions = 0;
      if (source) {
        permissions = access->capabilities.required_access;
        if (iree_any_bit_set(updates[i].access, IREE_HAL_MEMORY_ACCESS_READ)) {
          permissions |= AMDF_MEMORY_ACCESS_READ;
        }
        if (iree_any_bit_set(updates[i].access, IREE_HAL_MEMORY_ACCESS_WRITE)) {
          permissions |= AMDF_MEMORY_ACCESS_WRITE;
        }
      }
      native_updates[count++] = (amdf_memory_mapping_update_t){
          .target_byte_offset = updates[i].target_range.byte_offset,
          .byte_length = updates[i].target_range.byte_length,
          .memory = source ? source->memory : NULL,
          .memory_access_ordinal = source ? source->access_ordinal : 0,
          .access = permissions,
          .source_byte_offset = source
                                    ? updates[i].source.mapping_source.offset -
                                          source->logical_byte_offset
                                    : 0,
      };
    }
    if (count) {
      status = IREE_HAL_AMD_STATUS_FROM_AMDF(
          reservation->api->memory_reservation_update(reservation->native, a,
                                                      count, native_updates),
          "memory_reservation_update");
    }
  }
  if (native_updates != inline_updates) {
    iree_allocator_free(base->host_allocator, native_updates);
  }
  return status;
}

static const iree_hal_memory_reservation_vtable_t
    iree_hal_amd_fabric_memory_reservation_vtable = {
        .close = iree_hal_amd_fabric_memory_reservation_close,
        .destroy = iree_hal_amd_fabric_memory_reservation_destroy,
        .update = iree_hal_amd_fabric_memory_reservation_update,
};

iree_status_t iree_hal_amd_fabric_memory_reservation_create(
    const iree_hal_memory_backend_t* base, iree_hal_device_group_t* group,
    iree_hal_pool_scope_t scope,
    const iree_hal_memory_reservation_options_t* options,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t** out_reservation) {
  const iree_hal_amd_fabric_memory_backend_t* owner =
      (const iree_hal_amd_fabric_memory_backend_t*)base;
  if (!owner->native.instance || !scope.family_count || scope.host.access ||
      scope.family_count >= IREE_HAL_BUFFER_NATIVE_BINDING_INDEX_NONE) {
    return iree_ok_status();
  }
  const uint32_t scope_count = iree_hal_device_group_memory_scope_count(group);
  iree_host_size_t total_size = 0;
  iree_host_size_t accesses_offset = 0, scopes_offset = 0;
  iree_host_size_t projections_offset = 0, bindings_offset = 0;
  iree_host_size_t types_offset = 0, native_accesses_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_hal_amd_fabric_memory_reservation_t), &total_size,
      IREE_STRUCT_FIELD(scope.family_count,
                        iree_hal_amd_fabric_reservation_access_t,
                        &accesses_offset),
      IREE_STRUCT_FIELD(scope_count, uint32_t, &scopes_offset),
      IREE_STRUCT_FIELD(scope_count, uint32_t, &projections_offset),
      IREE_STRUCT_FIELD(scope.family_count, iree_hal_buffer_native_binding_t,
                        &bindings_offset),
      IREE_STRUCT_FIELD(scope.family_count, uint16_t, &types_offset),
      IREE_STRUCT_FIELD(scope.family_count, amdf_memory_reservation_access_t,
                        &native_accesses_offset)));
  iree_hal_amd_fabric_memory_reservation_t* reservation = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, total_size, (void**)&reservation));
  reservation->api = owner->native.api;
  reservation->accesses =
      (iree_hal_amd_fabric_reservation_access_t*)((uint8_t*)reservation +
                                                  accesses_offset);
  reservation->scope_accesses =
      (uint32_t*)((uint8_t*)reservation + scopes_offset);
  reservation->projections =
      (uint32_t*)((uint8_t*)reservation + projections_offset);
  reservation->bindings =
      (iree_hal_buffer_native_binding_t*)((uint8_t*)reservation +
                                          bindings_offset);
  uint16_t* types = (uint16_t*)((uint8_t*)reservation + types_offset);
  amdf_memory_reservation_access_t* native_accesses =
      (amdf_memory_reservation_access_t*)((uint8_t*)reservation +
                                          native_accesses_offset);
  for (uint32_t i = 0; i < scope_count; ++i) {
    reservation->scope_accesses[i] = IREE_HAL_MEMORY_SCOPE_INVALID;
    reservation->projections[i] = IREE_HAL_MEMORY_SCOPE_INVALID;
  }
  bool supported = true;
  iree_device_size_t granularity = iree_max(options->mapping_granularity, 1);
  iree_device_size_t alignment = options->min_alignment;
  iree_hal_buffer_usage_t usage = 0;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < scope.family_count && supported && iree_status_is_ok(status); ++i) {
    const iree_hal_pool_family_access_t* requested = &scope.families[i];
    const iree_hal_memory_backend_t* participant =
        iree_hal_device_memory_backend(
            iree_hal_queue_family_device(requested->family));
    const iree_hal_atomic_operation_capabilities_t no_atomics = {0};
    if (!participant ||
        participant->reserve != iree_hal_amd_fabric_memory_reservation_create ||
        requested->requirements ||
        !iree_hal_atomic_operation_capabilities_support(
            no_atomics, requested->atomic_operations) ||
        (requested->interfaces &
         ~(UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS))) {
      supported = false;
      break;
    }
    const iree_hal_amd_fabric_memory_backend_t* backend =
        (const iree_hal_amd_fabric_memory_backend_t*)participant;
    const iree_hal_amd_fabric_memory_family_t* family =
        &backend->native
             .families[iree_hal_queue_family_ordinal(requested->family)];
    bool gpu_address = false;
    for (uint32_t b = 0; b < family->binding_count; ++b) {
      gpu_address |=
          family->bindings[b].type ==
              IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS &&
          family->bindings[b].address_kind == AMDF_MEMORY_ADDRESS_GPU;
    }
    if (backend->native.api != owner->native.api ||
        backend->native.instance != owner->native.instance || !gpu_address ||
        (requested->usage & ~family->supported_usage) ||
        (requested->usage & family->host_usage)) {
      supported = false;
      break;
    }
    uint32_t a = 0;
    for (; a < reservation->access_count; ++a) {
      if (reservation->accesses[a].native.device == backend->native.device &&
          reservation->accesses[a].native.address_domain_ordinal ==
              backend->native.address_domain_ordinal) {
        break;
      }
    }
    iree_hal_amd_fabric_reservation_access_t* access =
        &reservation->accesses[a];
    if (a == reservation->access_count) {
      access->native.device = backend->native.device;
      access->native.address_domain_ordinal =
          backend->native.address_domain_ordinal;
      access->capabilities = (amdf_memory_reservation_capabilities_t){
          .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CAPABILITIES,
          .structure_size = sizeof(access->capabilities),
      };
      const amdf_status_t native_status =
          owner->native.api->device_query_memory_reservation_capabilities(
              backend->native.device, backend->native.address_domain_ordinal,
              &access->capabilities);
      if (native_status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
        supported = false;
        break;
      }
      status = IREE_HAL_AMD_STATUS_FROM_AMDF(
          native_status, "device_query_memory_reservation_capabilities");
      if (!iree_status_is_ok(status)) {
        break;
      }
      ++reservation->access_count;
      granularity =
          iree_max(granularity, access->capabilities.mapping_granularity);
      alignment =
          iree_max(alignment, access->capabilities.reservation_granularity);
      if (options->byte_length > access->capabilities.maximum_byte_length ||
          options->byte_length % access->capabilities.reservation_granularity) {
        status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                  "reservation length exceeds native geometry");
        break;
      }
    }
    const iree_hal_memory_access_t permissions =
        iree_hal_buffer_usage_access(requested->usage);
    access->native.access |=
        family->minimum_access | access->capabilities.required_access;
    if (iree_any_bit_set(permissions, IREE_HAL_MEMORY_ACCESS_READ)) {
      access->native.access |= AMDF_MEMORY_ACCESS_READ;
    }
    if (iree_any_bit_set(permissions, IREE_HAL_MEMORY_ACCESS_WRITE)) {
      access->native.access |= AMDF_MEMORY_ACCESS_WRITE;
    }
    supported &=
        (access->native.access & ~access->capabilities.supported_access) == 0;
    const uint32_t id = requested->family->memory.queue_scope_id;
    reservation->scope_accesses[id] = a;
    reservation->scope_accesses[id + 1] = a;
    usage |= requested->usage;
  }
  for (uint32_t a = 0;
       a < reservation->access_count && supported && iree_status_is_ok(status);
       ++a) {
    const iree_hal_amd_fabric_reservation_access_t* access =
        &reservation->accesses[a];
    if (alignment > access->capabilities.maximum_alignment ||
        options->byte_length % granularity) {
      status =
          iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                           "reservation alignment exceeds native geometry");
    }
    native_accesses[a] = access->native;
    types[a] = IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS;
  }
  iree_hal_memory_contract_t* contract = NULL;
  if (supported && iree_status_is_ok(status)) {
    const iree_hal_buffer_binding_layout_t layout = {
        .byte_length =
            reservation->access_count * sizeof(*reservation->bindings),
        .binding_count = (uint16_t)reservation->access_count,
        .host_binding_index = IREE_HAL_BUFFER_NATIVE_BINDING_INDEX_NONE,
        .types = types,
    };
    status = iree_hal_memory_contract_create(
        iree_hal_device_group_memory_domain(group), scope_count, &layout,
        host_allocator, &contract);
  }
  if (contract && iree_status_is_ok(status)) {
    const amdf_memory_reservation_create_info_t info = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CREATE_INFO,
        .structure_size = sizeof(info),
        .byte_length = options->byte_length,
        .minimum_alignment = alignment,
        .access_count = reservation->access_count,
        .accesses = native_accesses,
    };
    status = IREE_HAL_AMD_STATUS_FROM_AMDF(
        owner->native.api->memory_reservation_create(
            owner->native.instance, &info, &reservation->native),
        "memory_reservation_create");
  }
  uint32_t projection_count = 0;
  for (uint32_t a = 0; reservation->native && a < reservation->access_count &&
                       iree_status_is_ok(status);
       ++a) {
    iree_hal_amd_fabric_reservation_access_t* access =
        &reservation->accesses[a];
    access->info = (amdf_memory_reservation_access_info_t){
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO,
        .structure_size = sizeof(access->info),
    };
    status = IREE_HAL_AMD_STATUS_FROM_AMDF(
        reservation->api->memory_reservation_query_access_info(
            reservation->native, a, &access->info),
        "memory_reservation_query_access_info");
    if (iree_status_is_ok(status)) {
      access->projection =
          access->info.mapping_access_ordinal == a
              ? projection_count++
              : reservation->accesses[access->info.mapping_access_ordinal]
                    .projection;
      reservation->bindings[a].device_address = access->info.address;
    }
  }
  if (reservation->native && iree_status_is_ok(status)) {
    contract->allowed_usage = usage;
    contract->allowed_access = iree_hal_buffer_usage_access(usage);
    for (iree_host_size_t i = 0; i < scope.family_count; ++i) {
      const uint32_t id = scope.families[i].family->memory.queue_scope_id;
      const uint32_t a = reservation->scope_accesses[id];
      iree_hal_memory_scope_access_t* site = &contract->scopes[id];
      site->usage = scope.families[i].usage;
      site->interfaces = 1u << IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS;
      site->bindings[IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS] = (uint16_t)a;
      contract->scopes[id + 1] = *site;
      reservation->projections[id] = reservation->accesses[a].projection;
      reservation->projections[id + 1] = reservation->accesses[a].projection;
    }
    reservation->backing = (iree_hal_buffer_backing_facts_t){
        .notification = owner->notification,
        .tracker =
            iree_hal_device_topology_info(owner->device)->frontier.tracker,
        .maintenance = owner->maintenance,
        .maintenance_range = {.byte_length = options->byte_length},
        .allocation_alignment = alignment,
        .maintenance_alignment = 1,
    };
    iree_hal_memory_reservation_initialize(
        contract, reservation->bindings, &reservation->backing,
        IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE, options->byte_length, granularity,
        &iree_hal_amd_fabric_memory_reservation_vtable, host_allocator,
        &reservation->base);
    reservation->base.projection_count = projection_count;
    reservation->base.projections = reservation->projections;
    *out_reservation = &reservation->base;
  } else {
    if (reservation->native) {
      status = iree_status_join(
          status,
          IREE_HAL_AMD_STATUS_FROM_AMDF(
              reservation->api->memory_reservation_destroy(reservation->native),
              "memory_reservation_destroy"));
    }
    iree_allocator_free(host_allocator, reservation);
  }
  iree_hal_memory_contract_release(contract);
  return status;
}
