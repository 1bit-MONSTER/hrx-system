// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/firmware_trace.h"

#include <drm/amdxdna_accel.h>
#include <string.h>
#include <sys/ioctl.h>

#include "libamdf/src/platform/linux/file.h"
#include "libamdf/src/xdna/umd/drm/device.h"

// AMD SDK DPT requests extend the upstream DRM ABI. These layouts are local to
// this optional provider service; its admission is the native ioctl result.
typedef struct amdf_linux_xdna_trace_state_t {
  // Active payload schema; undefined when disabled.
  uint32_t version;
  // One for enabled, zero for disabled.
  uint32_t status;
  // Enabled category mask.
  uint32_t config;
  // Native reserved padding.
  uint32_t pad;
} amdf_linux_xdna_trace_state_t;

typedef struct amdf_linux_xdna_trace_configuration_t {
  // One enables, zero disables.
  uint32_t action;
  // Nonzero category mask when enabling.
  uint32_t config;
  // Native reserved padding.
  uint64_t pad;
} amdf_linux_xdna_trace_configuration_t;

typedef struct amdf_linux_xdna_trace_metadata_t {
  // Requested position, replaced by the native next read position.
  uint64_t offset;
  // Number of returned payload bytes.
  uint32_t size;
  // One requests a blocking native read; this service always passes zero.
  uint8_t watch;
  // Native reserved padding.
  uint8_t pad[3];
} amdf_linux_xdna_trace_metadata_t;

typedef struct amdf_linux_xdna_trace_array_t {
  // SDK GET_ARRAY selector.
  uint32_t param;
  // Native byte capacity of one element.
  uint32_t element_size;
  // One element is supplied.
  uint32_t num_element;
  // Native reserved padding.
  uint32_t pad;
  // User address of the element.
  uint64_t buffer;
} amdf_linux_xdna_trace_array_t;

#define AMDF_LINUX_XDNA_TRACE_GET_ARRAY \
  DRM_IOWR(DRM_COMMAND_BASE + 10, amdf_linux_xdna_trace_array_t)

static amdf_status_t amdf_linux_xdna_trace_error(int error) {
  return error == EOPNOTSUPP || error == ENOTTY
             ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
             : amdf_linux_error(error);
}

amdf_status_t amdf_xdna_umd_query_firmware_trace_info(
    amdf_xdna_umd_device_t* device, amdf_xdna_firmware_trace_info_t* out_info) {
  amdf_linux_xdna_trace_state_t state = {0};
  amdf_linux_xdna_trace_array_t request = {
      .param = 8,
      .element_size = sizeof(state),
      .num_element = 1,
      .buffer = (uint64_t)(uintptr_t)&state,
  };
  if (ioctl(device->descriptor, AMDF_LINUX_XDNA_TRACE_GET_ARRAY, &request) !=
      0) {
    return amdf_linux_xdna_trace_error(errno);
  }
  if (state.status > 1 || request.element_size != sizeof(state) ||
      request.num_element != 1) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  *out_info = (amdf_xdna_firmware_trace_info_t){
      .enabled = state.status,
      .categories = state.status ? state.config : 0,
      .format_version = state.status ? state.version : 0,
      .buffer_alignment = 8,
      .minimum_buffer_byte_length =
          sizeof(amdf_linux_xdna_trace_metadata_t) + 32,
      .recommended_buffer_byte_length =
          sizeof(amdf_linux_xdna_trace_metadata_t) + 4 * 1024 * 1024,
  };
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_configure_firmware_trace(
    amdf_xdna_umd_device_t* device, uint32_t enabled, uint32_t categories) {
  amdf_linux_xdna_trace_configuration_t configuration = {
      .action = enabled,
      .config = enabled ? categories : 0,
  };
  struct amdxdna_drm_set_state request = {
      .param = 6,
      .buffer_size = sizeof(configuration),
      .buffer = (uint64_t)(uintptr_t)&configuration,
  };
  return ioctl(device->descriptor, DRM_IOCTL_AMDXDNA_SET_STATE, &request) == 0
             ? AMDF_STATUS_OK
             : amdf_linux_xdna_trace_error(errno);
}

amdf_status_t amdf_xdna_umd_read_firmware_trace(
    amdf_xdna_umd_device_t* device, uint64_t position, void* buffer,
    uint32_t buffer_byte_length, amdf_observation_read_result_t* out_result) {
  if (buffer_byte_length < sizeof(amdf_linux_xdna_trace_metadata_t) + 32) {
    return amdf_make_api_status(AMDF_STATUS_CODE_BUFFER_TOO_SMALL);
  }
  const uint32_t capacity =
      (buffer_byte_length - sizeof(amdf_linux_xdna_trace_metadata_t)) & ~31u;
  amdf_linux_xdna_trace_metadata_t metadata = {.offset = position};
  memcpy((uint8_t*)buffer + capacity, &metadata, sizeof(metadata));
  amdf_linux_xdna_trace_array_t request = {
      .param = 4,
      .element_size = capacity + sizeof(metadata),
      .num_element = 1,
      .buffer = (uint64_t)(uintptr_t)buffer,
  };
  const int native_result =
      ioctl(device->descriptor, AMDF_LINUX_XDNA_TRACE_GET_ARRAY, &request);
  const int error = errno;
  if (native_result != 0 && error != ESTALE) {
    return amdf_linux_xdna_trace_error(error);
  }
  memcpy(&metadata, (uint8_t*)buffer + capacity, sizeof(metadata));
  amdf_observation_read_result_t result = {.next_position = metadata.offset};
  if (native_result != 0) {
    result.lost_byte_length = UINT64_MAX;
    result.flags =
        AMDF_OBSERVATION_READ_FLAG_RESTART | AMDF_OBSERVATION_READ_FLAG_GAP;
  } else {
    if (metadata.size > capacity || metadata.offset < position ||
        metadata.offset - position < metadata.size) {
      return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
    }
    result.byte_length = metadata.size;
    result.lost_byte_length = metadata.offset - position - metadata.size;
    if (result.lost_byte_length != 0) {
      result.flags = AMDF_OBSERVATION_READ_FLAG_GAP;
    }
  }
  *out_result = result;
  return AMDF_STATUS_OK;
}
