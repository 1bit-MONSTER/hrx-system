// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/fabric/fabric.h"

#include "iree/hal/drivers/amd/status.h"

void iree_hal_amd_fabric_retain(iree_hal_amd_fabric_t* fabric) {
  if (fabric) {
    iree_atomic_ref_count_inc(&fabric->ref_count);
  }
}

void iree_hal_amd_fabric_release(iree_hal_amd_fabric_t* fabric) {
  if (!fabric) {
    return;
  }
  if (iree_atomic_ref_count_dec(&fabric->ref_count) != 1) {
    return;
  }
  if (fabric->instance) {
    const amdf_status_t status =
        fabric->api->instance_destroy(fabric->instance);
    if (!amdf_status_is_ok(status)) {
      iree_hal_amd_report_failure(iree_hal_device_event_sink_stderr(),
                                  IREE_SV("amd"), status, "instance_destroy");
      return;
    }
  }
  iree_allocator_free(fabric->host_allocator, fabric->endpoints);
  iree_allocator_free(fabric->host_allocator, fabric);
}

static iree_status_t iree_hal_amd_fabric_discover(
    iree_hal_amd_fabric_t* fabric) {
  uint32_t count = 0;
  const amdf_status_t count_status =
      fabric->api->instance_enumerate_memory_scopes(fabric->instance, 0, NULL,
                                                    &count);
  if (count_status != amdf_make_api_status(AMDF_STATUS_CODE_BUFFER_TOO_SMALL)) {
    IREE_RETURN_IF_ERROR(IREE_HAL_AMD_STATUS_FROM_AMDF(
        count_status, "instance_enumerate_memory_scopes(count)"));
  }
  iree_host_size_t size = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      0, &size, IREE_STRUCT_FIELD_FAM(count, amdf_memory_scope_t*)));
  amdf_memory_scope_t** scopes = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(fabric->host_allocator, size, (void**)&scopes));
  iree_status_t status = IREE_HAL_AMD_STATUS_FROM_AMDF(
      fabric->api->instance_enumerate_memory_scopes(fabric->instance, count,
                                                    scopes, &count),
      "instance_enumerate_memory_scopes");
  for (uint32_t i = 0; i < count && iree_status_is_ok(status); ++i) {
    amdf_memory_scope_info_t info = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_SCOPE_INFO,
        .structure_size = sizeof(info),
    };
    status = IREE_HAL_AMD_STATUS_FROM_AMDF(
        fabric->api->memory_scope_query_info(scopes[i], &info),
        "memory_scope_query_info");
    if (iree_status_is_ok(status) &&
        info.kind == AMDF_MEMORY_SCOPE_KIND_SYSTEM) {
      fabric->system_scope = scopes[i];
      break;
    }
  }
  iree_allocator_free(fabric->host_allocator, scopes);
  IREE_RETURN_IF_ERROR(status);
  if (!fabric->system_scope) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE, "no system memory scope");
  }
  IREE_RETURN_IF_ERROR(IREE_HAL_AMD_STATUS_FROM_AMDF(
      fabric->api->endpoint_enumerate(fabric->instance, 0, NULL, &count),
      "endpoint_enumerate(count)"));
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      0, &size, IREE_STRUCT_FIELD_FAM(count, amdf_endpoint_summary_t)));
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(fabric->host_allocator, size,
                                             (void**)&fabric->endpoints));
  IREE_RETURN_IF_ERROR(IREE_HAL_AMD_STATUS_FROM_AMDF(
      fabric->api->endpoint_enumerate(fabric->instance, count,
                                      fabric->endpoints, &count),
      "endpoint_enumerate"));
  fabric->endpoint_count = count;
  return iree_ok_status();
}

iree_status_t iree_hal_amd_fabric_create(const amdf_api_t* api,
                                         amdf_native_lifetime_t native_lifetime,
                                         iree_allocator_t host_allocator,
                                         iree_hal_amd_fabric_t** out_fabric) {
  *out_fabric = NULL;
  iree_hal_amd_fabric_t* fabric = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*fabric), (void**)&fabric));
  iree_atomic_ref_count_init(&fabric->ref_count);
  fabric->host_allocator = host_allocator;
  fabric->api = api;
  const amdf_instance_create_info_t create_info = {
      .type = AMDF_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .structure_size = sizeof(create_info),
      .native_lifetime = native_lifetime,
  };
  iree_status_t status = IREE_HAL_AMD_STATUS_FROM_AMDF(
      api->instance_create(&create_info, &fabric->instance), "instance_create");
  if (iree_status_is_ok(status)) {
    status = iree_hal_amd_fabric_discover(fabric);
  }
  if (iree_status_is_ok(status)) {
    *out_fabric = fabric;
  } else {
    iree_hal_amd_fabric_release(fabric);
  }
  return status;
}
