// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/fabric/memory.h"

#include "iree/hal/drivers/amd/status.h"

void iree_hal_amd_fabric_memory_deinitialize(
    const amdf_api_t* api, iree_hal_device_event_sink_t event_sink,
    iree_hal_amd_fabric_memory_t* memory) {
  if (memory->mapping) {
    amdf_status_t status = api->host_mapping_destroy(memory->mapping);
    if (!amdf_status_is_ok(status)) {
      iree_hal_amd_report_failure(event_sink, IREE_SV("amd"), status,
                                  "host_mapping_destroy");
      return;
    }
  }
  if (memory->handle) {
    // memory_destroy consumes its handle even on native cleanup failure; the
    // provider preserves any backing required by failed detach as a leak.
    amdf_status_t status = api->memory_destroy(memory->handle);
    if (!amdf_status_is_ok(status)) {
      iree_hal_amd_report_failure(event_sink, IREE_SV("amd"), status,
                                  "memory_destroy");
    }
  }
  *memory = (iree_hal_amd_fabric_memory_t){0};
}

iree_status_t iree_hal_amd_fabric_memory_allocate(
    const amdf_api_t* api, const iree_hal_amd_fabric_memory_options_t* options,
    uint64_t byte_length, uint64_t alignment,
    iree_hal_device_event_sink_t event_sink,
    iree_hal_amd_fabric_memory_t* out_memory) {
  *out_memory = (iree_hal_amd_fabric_memory_t){0};
  const uint64_t granularity =
      options->profile.allocation.byte_length_granularity;
  uint64_t allocation_length = 0;
  if (!iree_checked_add_u64(iree_max(byte_length, 1), granularity - 1,
                            &allocation_length) ||
      (options->host_access && allocation_length > IREE_HOST_SIZE_MAX)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "AMD allocation extent overflows host size");
  }
  allocation_length = allocation_length / granularity * granularity;
  if (allocation_length > options->profile.allocation.maximum_byte_length) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "AMD allocation exceeds the native profile");
  }
  const amdf_memory_create_info_t create_info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO,
      .structure_size = sizeof(create_info),
      .memory_profile_ordinal = options->profile.ordinal,
      .access_count = options->access_count,
      .required_flags = options->required_flags,
      .byte_length = allocation_length,
      .minimum_alignment =
          iree_max(alignment, options->profile.allocation.minimum_alignment),
      .accesses = options->accesses,
  };
  iree_hal_amd_fabric_memory_t memory = {0};
  IREE_RETURN_IF_ERROR(IREE_HAL_AMD_STATUS_FROM_AMDF(
      api->memory_create(options->scope, &create_info, &memory.handle),
      "memory_create"));
  if (!options->host_access) {
    *out_memory = memory;
    return iree_ok_status();
  }
  const amdf_memory_map_info_t map_info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO,
      .structure_size = sizeof(map_info),
      .byte_length = allocation_length,
      .flags = options->host_access,
  };
  iree_status_t status = IREE_HAL_AMD_STATUS_FROM_AMDF(
      api->memory_map(memory.handle, &map_info, &memory.mapping), "memory_map");
  amdf_host_mapping_info_t mapping_info = {
      .type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO,
      .structure_size = sizeof(mapping_info),
  };
  if (iree_status_is_ok(status)) {
    status = IREE_HAL_AMD_STATUS_FROM_AMDF(
        api->host_mapping_query_info(memory.mapping, &mapping_info),
        "host_mapping_query_info");
  }
  if (iree_status_is_ok(status)) {
    memory.contents = iree_make_byte_span(mapping_info.pointer, byte_length);
    memory.host_cacheability = mapping_info.cacheability;
    *out_memory = memory;
  } else {
    iree_hal_amd_fabric_memory_deinitialize(api, event_sink, &memory);
  }
  return status;
}
