// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/driver.h"

#include "iree/hal/drivers/amd/fabric/fabric.h"
#include "iree/hal/drivers/amd/status.h"
#include "iree/hal/drivers/amd/xdna/device.h"

typedef struct iree_hal_amd_driver_t {
  // HAL resource header.
  iree_hal_resource_t resource;
  // Allocator owning this driver.
  iree_allocator_t host_allocator;
  // Shared instance and passive discovery retained by each materialized device.
  iree_hal_amd_fabric_t* fabric;
} iree_hal_amd_driver_t;

static const iree_hal_driver_vtable_t iree_hal_amd_driver_vtable;

static void iree_hal_amd_driver_destroy(iree_hal_driver_t* base) {
  iree_hal_amd_driver_t* driver = (iree_hal_amd_driver_t*)base;
  iree_hal_amd_fabric_release(driver->fabric);
  iree_allocator_free(driver->host_allocator, driver);
}

iree_status_t iree_hal_amd_driver_create(iree_allocator_t host_allocator,
                                         iree_hal_driver_t** out_driver) {
  *out_driver = NULL;
  const amdf_api_t* api = NULL;
  IREE_RETURN_IF_ERROR(IREE_HAL_AMD_STATUS_FROM_AMDF(
      amdf_query_api(AMDF_ABI_VERSION_LATEST, AMDF_ABI_VERSION_LATEST, &api),
      "query_api"));
  iree_hal_amd_driver_t* driver = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*driver), (void**)&driver));
  iree_hal_resource_initialize(&iree_hal_amd_driver_vtable, &driver->resource);
  driver->host_allocator = host_allocator;
  iree_status_t status =
      iree_hal_amd_fabric_create(api, host_allocator, &driver->fabric);
  if (iree_status_is_ok(status)) {
    *out_driver = (iree_hal_driver_t*)driver;
  } else {
    iree_hal_driver_release((iree_hal_driver_t*)driver);
  }
  return status;
}

// Schemes select execution implementations, independently of physical endpoint
// enumeration. A driver never advertises devices it has no engine to execute.
static iree_status_t iree_hal_amd_driver_select_engine(
    iree_string_view_t driver_name, amdf_engine_kind_t* out_engine_kind) {
  if (iree_string_view_is_empty(driver_name) ||
      iree_string_view_equal(driver_name, IREE_SV("xdna"))) {
    *out_engine_kind = AMDF_ENGINE_KIND_XDNA;
    return iree_ok_status();
  }
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "AMD execution scheme '%.*s' is not built in",
                          (int)driver_name.size, driver_name.data);
}

static iree_status_t iree_hal_amd_driver_query_available_devices(
    iree_hal_driver_t* base, iree_string_view_t driver_name,
    iree_allocator_t host_allocator, iree_host_size_t* out_count,
    iree_hal_device_info_t** out_infos) {
  iree_hal_amd_driver_t* driver = (iree_hal_amd_driver_t*)base;
  const iree_hal_amd_fabric_t* fabric = driver->fabric;
  amdf_engine_kind_t engine_kind = AMDF_ENGINE_KIND_UNKNOWN;
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_driver_select_engine(driver_name, &engine_kind));
  uint32_t count = 0;
  for (uint32_t i = 0; i < fabric->endpoint_count; ++i) {
    count += fabric->endpoints[i].engine_kind == engine_kind;
  }
  if (!count) {
    return iree_ok_status();
  }
  // An endpoint identity path is "id/" followed by two 16-digit hex words.
  const iree_host_size_t path_capacity = 36;
  const iree_host_size_t string_capacity =
      AMDF_ENDPOINT_NAME_CAPACITY + path_capacity;
  iree_host_size_t size = 0, string_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      0, &size, IREE_STRUCT_FIELD_FAM(count, iree_hal_device_info_t),
      IREE_STRUCT_FIELD(count * string_capacity, char, &string_offset)));
  iree_hal_device_info_t* infos = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, size, (void**)&infos));
  char* data = (char*)infos + string_offset;
  uint32_t index = 0;
  for (uint32_t i = 0; i < fabric->endpoint_count; ++i) {
    const amdf_endpoint_summary_t* endpoint = &fabric->endpoints[i];
    if (endpoint->engine_kind != engine_kind) {
      continue;
    }
    const int path_length =
        iree_snprintf(data, path_capacity, "id/%016" PRIx64 "%016" PRIx64,
                      endpoint->id.words[0], endpoint->id.words[1]);
    infos[index].device_id = (iree_hal_device_id_t)i + 1;
    infos[index].path = iree_make_string_view(data, path_length);
    data += path_capacity;
    iree_string_view_append_to_buffer(iree_make_cstring_view(endpoint->name),
                                      &infos[index].name, data);
    data += AMDF_ENDPOINT_NAME_CAPACITY;
    ++index;
  }
  *out_count = count;
  *out_infos = infos;
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_driver_resolve_id(
    const iree_hal_amd_fabric_t* fabric, iree_hal_device_id_t id,
    const amdf_endpoint_summary_t** out_endpoint) {
  if (id == IREE_HAL_DEVICE_ID_DEFAULT) {
    for (uint32_t i = 0; i < fabric->endpoint_count; ++i) {
      if (fabric->endpoints[i].engine_kind == AMDF_ENGINE_KIND_XDNA) {
        *out_endpoint = &fabric->endpoints[i];
        return iree_ok_status();
      }
    }
  } else if (id - 1 < fabric->endpoint_count &&
             fabric->endpoints[id - 1].engine_kind == AMDF_ENGINE_KIND_XDNA) {
    *out_endpoint = &fabric->endpoints[id - 1];
    return iree_ok_status();
  }
  return iree_make_status(IREE_STATUS_NOT_FOUND, "AMD endpoint is unavailable");
}

static iree_status_t iree_hal_amd_driver_dump_device_info(
    iree_hal_driver_t* base, iree_hal_device_id_t id,
    iree_string_builder_t* builder) {
  iree_hal_amd_driver_t* driver = (iree_hal_amd_driver_t*)base;
  const amdf_endpoint_summary_t* endpoint = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_driver_resolve_id(driver->fabric, id, &endpoint));
  return iree_string_builder_append_format(builder, "AMD endpoint: %s\n",
                                           endpoint->name);
}

static iree_status_t iree_hal_amd_driver_create_device_by_id(
    iree_hal_driver_t* base, iree_hal_device_id_t id,
    iree_host_size_t param_count, const iree_string_pair_t* params,
    const iree_hal_device_create_params_t* create_params,
    iree_allocator_t host_allocator, iree_hal_device_t** out_device) {
  iree_hal_amd_driver_t* driver = (iree_hal_amd_driver_t*)base;
  const amdf_endpoint_summary_t* endpoint = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_driver_resolve_id(driver->fabric, id, &endpoint));
  iree_hal_amd_xdna_context_t* context = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_amd_xdna_context_create(
      driver->fabric, &endpoint->id, param_count, params,
      create_params->event_sink, host_allocator, &context));
  iree_status_t status = iree_hal_amd_xdna_device_create(
      context, iree_make_cstring_view(endpoint->name), create_params,
      host_allocator, out_device);
  if (!iree_status_is_ok(status)) {
    iree_hal_amd_xdna_context_destroy(context);
  }
  return status;
}

static iree_status_t iree_hal_amd_driver_create_device_by_path(
    iree_hal_driver_t* base, iree_string_view_t driver_name,
    iree_string_view_t path, iree_host_size_t param_count,
    const iree_string_pair_t* params,
    const iree_hal_device_create_params_t* create_params,
    iree_allocator_t host_allocator, iree_hal_device_t** out_device) {
  iree_hal_amd_driver_t* driver = (iree_hal_amd_driver_t*)base;
  amdf_engine_kind_t engine_kind = AMDF_ENGINE_KIND_UNKNOWN;
  IREE_RETURN_IF_ERROR(
      iree_hal_amd_driver_select_engine(driver_name, &engine_kind));
  const bool by_identity =
      iree_string_view_consume_prefix(&path, IREE_SV("id/"));
  uint32_t ordinal = 0;
  amdf_endpoint_id_t endpoint_id = {{0, 0}};
  if (by_identity) {
    if (path.size != 32 ||
        !iree_string_view_atoi_uint64_base(iree_string_view_substr(path, 0, 16),
                                           16, &endpoint_id.words[0]) ||
        !iree_string_view_atoi_uint64_base(
            iree_string_view_substr(path, 16, 16), 16, &endpoint_id.words[1])) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "AMD identity path must be id/<32 hexadecimal digits>");
    }
  } else if (!iree_string_view_is_empty(path) &&
             !iree_string_view_atoi_uint32_base(path, 10, &ordinal)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "AMD device path must be a family ordinal or id/<identity>");
  }
  uint32_t family_ordinal = 0;
  for (uint32_t i = 0; i < driver->fabric->endpoint_count; ++i) {
    const amdf_endpoint_summary_t* endpoint = &driver->fabric->endpoints[i];
    if (endpoint->engine_kind != engine_kind) {
      continue;
    }
    if (by_identity ? amdf_endpoint_id_is_equal(&endpoint->id, &endpoint_id)
                    : family_ordinal == ordinal) {
      return iree_hal_amd_driver_create_device_by_id(
          base, (iree_hal_device_id_t)i + 1, param_count, params, create_params,
          host_allocator, out_device);
    }
    ++family_ordinal;
  }
  return iree_make_status(
      IREE_STATUS_NOT_FOUND,
      "no AMD endpoint matches the selected family and path");
}

static const iree_hal_driver_vtable_t iree_hal_amd_driver_vtable = {
    .destroy = iree_hal_amd_driver_destroy,
    .query_available_devices = iree_hal_amd_driver_query_available_devices,
    .dump_device_info = iree_hal_amd_driver_dump_device_info,
    .create_device_by_id = iree_hal_amd_driver_create_device_by_id,
    .create_device_by_path = iree_hal_amd_driver_create_device_by_path,
};
