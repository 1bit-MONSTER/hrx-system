// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/fabric/slab_provider.h"

#include "iree/base/internal/math.h"
#include "iree/hal/drivers/amd/status.h"

typedef struct iree_hal_amd_fabric_slab_provider_t {
  // Base slab-provider interface header.
  iree_hal_slab_provider_t base;
  // Host allocator owning this provider and slab metadata.
  iree_allocator_t host_allocator;
  // Borrowed HAL device used for materialized buffer placement.
  iree_hal_device_t* device;
  // Borrowed native core API, dominated by the sealed device group.
  const amdf_api_t* api;
  // Borrowed diagnostic destination for native cleanup failures.
  iree_hal_device_event_sink_t event_sink;
  // Qualified construction contract with provider-owned array storage.
  iree_hal_amd_fabric_slab_provider_options_t options;
  // Immutable HAL capabilities of all slabs.
  iree_hal_slab_provider_properties_t properties;
  // Cumulative successful slab acquisitions.
  iree_atomic_int64_t total_acquired;
  // Cumulative slab release attempts.
  iree_atomic_int64_t total_released;
} iree_hal_amd_fabric_slab_provider_t;

typedef struct iree_hal_amd_fabric_slab_handle_t {
  // Native allocation and optional persistent host mapping.
  iree_hal_amd_fabric_memory_t memory;
  // Prepared addresses in the contract layout order.
  iree_hal_buffer_native_binding_t* bindings;
} iree_hal_amd_fabric_slab_handle_t;

static const iree_hal_slab_provider_vtable_t
    iree_hal_amd_fabric_slab_provider_vtable;

static iree_hal_amd_fabric_slab_provider_t*
iree_hal_amd_fabric_slab_provider_cast(
    iree_hal_slab_provider_t* base_provider) {
  return (iree_hal_amd_fabric_slab_provider_t*)base_provider;
}

static const iree_hal_amd_fabric_slab_provider_t*
iree_hal_amd_fabric_slab_provider_const_cast(
    const iree_hal_slab_provider_t* base_provider) {
  return (const iree_hal_amd_fabric_slab_provider_t*)base_provider;
}

static iree_hal_pool_export_classes_t iree_hal_amd_fabric_copy_exports(
    iree_hal_pool_export_classes_t source,
    iree_hal_external_storage_class_t** cursor) {
  iree_hal_pool_export_classes_t result = {.class_count = source.class_count};
  if (source.class_count) {
    result.classes = *cursor;
    memcpy(*cursor, source.classes, source.class_count * sizeof(**cursor));
    *cursor += source.class_count;
  }
  return result;
}

iree_status_t iree_hal_amd_fabric_slab_provider_create(
    iree_hal_device_t* device, const amdf_api_t* api,
    iree_hal_device_event_sink_t event_sink,
    const iree_hal_amd_fabric_slab_provider_options_t* options,
    iree_allocator_t host_allocator, iree_hal_slab_provider_t** out_provider) {
  *out_provider = NULL;
  iree_host_size_t total_size = 0;
  iree_host_size_t accesses_offset = 0;
  iree_host_size_t bindings_offset = 0;
  iree_host_size_t imports_offset = 0;
  iree_host_size_t exports_offset = 0;
  iree_host_size_t export_count = options->memory.exports.class_count;
  for (iree_host_size_t i = 0; i < options->import_count; ++i) {
    if (!iree_host_size_checked_add(
            export_count, options->imports[i].policy.exports.class_count,
            &export_count)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "AMD export class count overflows host size");
    }
  }
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_hal_amd_fabric_slab_provider_t), &total_size,
      IREE_STRUCT_FIELD_ALIGNED(
          options->memory.access_count, amdf_memory_device_access_t,
          iree_alignof(amdf_memory_device_access_t), &accesses_offset),
      IREE_STRUCT_FIELD_ALIGNED(
          options->binding_count, iree_hal_amd_fabric_binding_request_t,
          iree_alignof(iree_hal_amd_fabric_binding_request_t),
          &bindings_offset),
      IREE_STRUCT_FIELD(options->import_count,
                        iree_hal_amd_fabric_import_route_t, &imports_offset),
      IREE_STRUCT_FIELD(export_count, iree_hal_external_storage_class_t,
                        &exports_offset)));
  iree_hal_amd_fabric_slab_provider_t* provider = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, total_size, (void**)&provider));
  memset(provider, 0, total_size);
  iree_hal_slab_provider_initialize(&iree_hal_amd_fabric_slab_provider_vtable,
                                    &provider->base);
  provider->host_allocator = host_allocator;
  provider->device = device;
  provider->api = api;
  provider->event_sink = event_sink;
  provider->options = *options;
  amdf_memory_device_access_t* accesses =
      (amdf_memory_device_access_t*)((uint8_t*)provider + accesses_offset);
  memcpy(accesses, options->memory.accesses,
         options->memory.access_count * sizeof(*accesses));
  provider->options.memory.accesses = accesses;
  iree_hal_amd_fabric_binding_request_t* bindings =
      (iree_hal_amd_fabric_binding_request_t*)((uint8_t*)provider +
                                               bindings_offset);
  memcpy(bindings, options->bindings,
         options->binding_count * sizeof(*bindings));
  provider->options.bindings = bindings;
  iree_hal_external_storage_class_t* exports =
      (iree_hal_external_storage_class_t*)((uint8_t*)provider + exports_offset);
  provider->options.memory.exports =
      iree_hal_amd_fabric_copy_exports(options->memory.exports, &exports);
  iree_hal_amd_fabric_import_route_t* imports =
      (iree_hal_amd_fabric_import_route_t*)((uint8_t*)provider +
                                            imports_offset);
  for (iree_host_size_t i = 0; i < options->import_count; ++i) {
    imports[i] = options->imports[i];
    imports[i].policy.exports = iree_hal_amd_fabric_copy_exports(
        options->imports[i].policy.exports, &exports);
  }
  provider->options.imports = imports;
  const bool owned = iree_all_bits_set(options->memory.profile.roles,
                                       AMDF_MEMORY_PROFILE_ROLE_CREATE);
  provider->properties = (iree_hal_slab_provider_properties_t){
      .memory_type = options->memory_type,
      .supported_usage = options->supported_usage,
      .queue_family_affinity = IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY,
      .allocation_alignment =
          owned ? options->memory.profile.allocation.minimum_alignment : 1,
      .max_allocation_alignment =
          owned ? options->memory.profile.allocation.maximum_alignment : 1,
      .maintenance_alignment = options->maintenance_alignment,
  };
  *out_provider = &provider->base;
  return iree_ok_status();
}

static void iree_hal_amd_fabric_slab_provider_destroy(
    iree_hal_slab_provider_t* base_provider) {
  iree_hal_amd_fabric_slab_provider_t* provider =
      iree_hal_amd_fabric_slab_provider_cast(base_provider);
  iree_allocator_free(provider->host_allocator, provider);
}

static iree_status_t iree_hal_amd_fabric_slab_handle_create(
    iree_hal_amd_fabric_slab_provider_t* provider,
    iree_hal_amd_fabric_slab_handle_t** out_handle) {
  const uint32_t binding_count = provider->options.binding_count;
  iree_host_size_t total_size = 0;
  iree_host_size_t bindings_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_hal_amd_fabric_slab_handle_t), &total_size,
      IREE_STRUCT_FIELD_ALIGNED(binding_count, iree_hal_buffer_native_binding_t,
                                iree_alignof(iree_hal_buffer_native_binding_t),
                                &bindings_offset)));
  iree_hal_amd_fabric_slab_handle_t* handle = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(provider->host_allocator,
                                             total_size, (void**)&handle));
  memset(handle, 0, total_size);
  handle->bindings =
      (iree_hal_buffer_native_binding_t*)((uint8_t*)handle + bindings_offset);
  *out_handle = handle;
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_fabric_slab_handle_bind(
    iree_hal_amd_fabric_slab_provider_t* provider,
    iree_hal_amd_fabric_slab_handle_t* handle) {
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0;
       i < provider->options.binding_count && iree_status_is_ok(status); ++i) {
    if (i == provider->options.host_binding_index) {
      handle->bindings[i].host_pointer = handle->memory.contents.data;
    } else {
      const iree_hal_amd_fabric_binding_request_t* binding =
          &provider->options.bindings[i];
      status = IREE_HAL_AMD_STATUS_FROM_AMDF(
          provider->api->memory_query_address(
              handle->memory.handle, binding->access_ordinal,
              binding->address_kind, &handle->bindings[i].device_address),
          "memory_query_address");
    }
  }
  return status;
}

static iree_status_t iree_hal_amd_fabric_slab_provider_acquire_slab(
    iree_hal_slab_provider_t* base_provider, iree_device_size_t min_length,
    iree_device_size_t min_alignment, iree_hal_slab_t* out_slab) {
  iree_hal_amd_fabric_slab_provider_t* provider =
      iree_hal_amd_fabric_slab_provider_cast(base_provider);
  memset(out_slab, 0, sizeof(*out_slab));
  if (!min_length) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "AMD slab allocations must be non-empty");
  }
  if (!iree_all_bits_set(provider->options.memory.profile.roles,
                         AMDF_MEMORY_PROFILE_ROLE_CREATE)) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "AMD provider admits imported storage only");
  }
  iree_hal_amd_fabric_slab_handle_t* handle = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_fabric_slab_handle_create(provider, &handle));
  iree_status_t status = iree_hal_amd_fabric_memory_allocate(
      provider->api, &provider->options.memory, min_length, min_alignment,
      provider->event_sink, &handle->memory);
  if (iree_status_is_ok(status)) {
    status = iree_hal_amd_fabric_slab_handle_bind(provider, handle);
  }
  if (iree_status_is_ok(status)) {
    out_slab->base_ptr = handle->memory.contents.data;
    out_slab->length = min_length;
    out_slab->provider_handle = (uint64_t)(uintptr_t)handle;
    iree_atomic_fetch_add(&provider->total_acquired, 1,
                          iree_memory_order_relaxed);
  } else {
    iree_hal_amd_fabric_memory_deinitialize(provider->api, provider->event_sink,
                                            &handle->memory);
    iree_allocator_free(provider->host_allocator, handle);
  }
  return status;
}

static const iree_hal_amd_fabric_import_route_t*
iree_hal_amd_fabric_slab_provider_find_import(
    const iree_hal_amd_fabric_slab_provider_t* provider,
    iree_hal_external_storage_class_t storage_class) {
  for (iree_host_size_t i = 0; i < provider->options.import_count; ++i) {
    const iree_hal_amd_fabric_import_route_t* route =
        &provider->options.imports[i];
    if (iree_hal_external_storage_class_equal(route->policy.storage_class,
                                              storage_class)) {
      return route;
    }
  }
  return NULL;
}

static iree_status_t iree_hal_amd_fabric_slab_provider_import_slab(
    iree_hal_slab_provider_t* base_provider,
    const iree_hal_external_storage_t* storage,
    iree_hal_imported_slab_t* out_import) {
  iree_hal_amd_fabric_slab_provider_t* provider =
      iree_hal_amd_fabric_slab_provider_cast(base_provider);
  const iree_hal_amd_fabric_import_route_t* route =
      iree_hal_amd_fabric_slab_provider_find_import(provider,
                                                    storage->storage_class);
  if (!route) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "external storage class was not admitted");
  }
  iree_hal_amd_fabric_slab_handle_t* handle = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_fabric_slab_handle_create(provider, &handle));
  iree_hal_amd_fabric_memory_options_t options = provider->options.memory;
  options.profile = route->profile;
  options.required_flags = route->required_flags;
  options.exports = route->policy.exports;
  iree_status_t status =
      storage->storage_class.type == IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER
          ? iree_hal_amd_fabric_memory_register(provider->api, &options,
                                                storage, provider->event_sink,
                                                &handle->memory)
          : iree_hal_amd_fabric_memory_import(provider->api, &options, storage,
                                              provider->event_sink,
                                              &handle->memory);
  if (iree_status_is_ok(status)) {
    status = iree_hal_amd_fabric_slab_handle_bind(provider, handle);
  }
  if (iree_status_is_ok(status)) {
    out_import->slab = (iree_hal_slab_t){
        .base_ptr = handle->memory.contents.data,
        .length = storage->byte_length,
        .provider_handle = (uint64_t)(uintptr_t)handle,
    };
    out_import->properties = provider->properties;
    out_import->properties.allocation_alignment = handle->memory.info.alignment;
    out_import->properties.max_allocation_alignment =
        handle->memory.info.alignment;
    iree_atomic_fetch_add(&provider->total_acquired, 1,
                          iree_memory_order_relaxed);
  } else {
    iree_hal_amd_fabric_memory_deinitialize(provider->api, provider->event_sink,
                                            &handle->memory);
    iree_allocator_free(provider->host_allocator, handle);
  }
  return status;
}

static bool iree_hal_amd_fabric_slab_provider_query_import_requirements(
    const iree_hal_slab_provider_t* base_provider,
    iree_hal_external_storage_class_t storage_class,
    iree_hal_pool_import_requirements_t* out_requirements) {
  const iree_hal_amd_fabric_import_route_t* route =
      iree_hal_amd_fabric_slab_provider_find_import(
          iree_hal_amd_fabric_slab_provider_const_cast(base_provider),
          storage_class);
  if (!route) {
    return false;
  }
  const bool registered =
      storage_class.type == IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER;
  const amdf_memory_construction_capabilities_t* capabilities =
      registered ? &route->profile.registration : &route->profile.import;
  *out_requirements = (iree_hal_pool_import_requirements_t){
      .min_byte_length = capabilities->byte_length_granularity,
      .max_byte_length = capabilities->maximum_byte_length,
      .base_alignment =
          registered ? capabilities->registered_host_pointer_alignment : 1,
      .byte_length_granularity = capabilities->byte_length_granularity,
  };
  return true;
}

static void iree_hal_amd_fabric_slab_provider_release_slab(
    iree_hal_slab_provider_t* base_provider, const iree_hal_slab_t* slab) {
  iree_hal_amd_fabric_slab_provider_t* provider =
      iree_hal_amd_fabric_slab_provider_cast(base_provider);
  iree_hal_amd_fabric_slab_handle_t* handle =
      (iree_hal_amd_fabric_slab_handle_t*)(uintptr_t)slab->provider_handle;
  if (!handle) {
    return;
  }
  iree_hal_amd_fabric_memory_deinitialize(provider->api, provider->event_sink,
                                          &handle->memory);
  iree_allocator_free(provider->host_allocator, handle);
  iree_atomic_fetch_add(&provider->total_released, 1,
                        iree_memory_order_relaxed);
}

static iree_status_t iree_hal_amd_fabric_slab_provider_wrap_buffer(
    iree_hal_slab_provider_t* base_provider, const iree_hal_slab_t* slab,
    iree_device_size_t slab_offset, iree_device_size_t allocation_size,
    iree_hal_memory_access_t allowed_access,
    iree_hal_buffer_usage_t allowed_usage,
    iree_hal_buffer_release_callback_t release_callback,
    iree_hal_buffer_t** out_buffer) {
  iree_hal_amd_fabric_slab_provider_t* provider =
      iree_hal_amd_fabric_slab_provider_cast(base_provider);
  iree_hal_amd_fabric_slab_handle_t* handle =
      (iree_hal_amd_fabric_slab_handle_t*)(uintptr_t)slab->provider_handle;
  const iree_hal_buffer_placement_t placement = {
      .device = provider->device,
      .queue_family_affinity = provider->properties.queue_family_affinity,
  };
  return iree_hal_amd_fabric_buffer_wrap(
      placement, provider->api,
      (iree_hal_amd_fabric_buffer_storage_t){
          .memory = &handle->memory,
          .bindings = handle->bindings,
          .host_binding_index = provider->options.host_binding_index,
          .offset = slab_offset,
      },
      allocation_size, provider->properties.memory_type, allowed_access,
      allowed_usage, release_callback, provider->host_allocator, out_buffer);
}

static iree_status_t iree_hal_amd_fabric_slab_provider_validate_asan_options(
    const iree_hal_slab_provider_t* base_provider,
    const iree_hal_asan_pool_options_t* options) {
  (void)base_provider;
  (void)options;
  return iree_make_status(
      IREE_STATUS_FAILED_PRECONDITION,
      "AMD AMDF slab provider does not support HAL ASAN range advice");
}

static void iree_hal_amd_fabric_slab_provider_advise_asan_range(
    iree_hal_slab_provider_t* base_provider, const iree_hal_slab_t* slab,
    iree_device_size_t backing_offset,
    iree_hal_asan_range_advice_flags_t advice_flags,
    const iree_hal_asan_allocation_layout_t* layout) {
  (void)base_provider;
  (void)slab;
  (void)backing_offset;
  (void)advice_flags;
  (void)layout;
  IREE_ASSERT(false, "AMD AMDF slab provider cannot advise ASAN ranges");
}

static void iree_hal_amd_fabric_slab_provider_prefault(
    iree_hal_slab_provider_t* base_provider, const iree_hal_slab_t* slab,
    iree_device_size_t offset, iree_device_size_t length) {
  (void)base_provider;
  (void)slab;
  (void)offset;
  (void)length;
}

static void iree_hal_amd_fabric_slab_provider_trim(
    iree_hal_slab_provider_t* base_provider, iree_hal_pool_trim_flags_t flags) {
  (void)base_provider;
  (void)flags;
}

static void iree_hal_amd_fabric_slab_provider_query_stats(
    const iree_hal_slab_provider_t* base_provider,
    iree_hal_slab_provider_visited_set_t* visited,
    iree_hal_slab_provider_stats_t* out_stats) {
  if (iree_hal_slab_provider_visited(visited, base_provider)) {
    return;
  }
  const iree_hal_amd_fabric_slab_provider_t* provider =
      iree_hal_amd_fabric_slab_provider_const_cast(base_provider);
  out_stats->total_acquired += (uint64_t)iree_atomic_load(
      &provider->total_acquired, iree_memory_order_relaxed);
  out_stats->total_released += (uint64_t)iree_atomic_load(
      &provider->total_released, iree_memory_order_relaxed);
}

static void iree_hal_amd_fabric_slab_provider_query_properties(
    const iree_hal_slab_provider_t* base_provider,
    iree_hal_slab_provider_properties_t* out_properties) {
  const iree_hal_amd_fabric_slab_provider_t* provider =
      iree_hal_amd_fabric_slab_provider_const_cast(base_provider);
  *out_properties = provider->properties;
}

static const iree_hal_slab_provider_vtable_t
    iree_hal_amd_fabric_slab_provider_vtable = {
        .destroy = iree_hal_amd_fabric_slab_provider_destroy,
        .acquire_slab = iree_hal_amd_fabric_slab_provider_acquire_slab,
        .import_slab = iree_hal_amd_fabric_slab_provider_import_slab,
        .release_slab = iree_hal_amd_fabric_slab_provider_release_slab,
        .wrap_buffer = iree_hal_amd_fabric_slab_provider_wrap_buffer,
        .validate_asan_options =
            iree_hal_amd_fabric_slab_provider_validate_asan_options,
        .advise_asan_range =
            iree_hal_amd_fabric_slab_provider_advise_asan_range,
        .prefault = iree_hal_amd_fabric_slab_provider_prefault,
        .trim = iree_hal_amd_fabric_slab_provider_trim,
        .query_stats = iree_hal_amd_fabric_slab_provider_query_stats,
        .query_properties = iree_hal_amd_fabric_slab_provider_query_properties,
        .query_import_requirements =
            iree_hal_amd_fabric_slab_provider_query_import_requirements,
};
