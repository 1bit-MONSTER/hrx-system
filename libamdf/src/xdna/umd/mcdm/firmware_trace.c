// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/firmware_trace.h"

#include <string.h>

#include "libamdf/src/xdna/umd/mcdm/context.h"
#include "libamdf/src/xdna/umd/mcdm/device.h"

enum {
  AMDF_WINDOWS_XDNA_TRACE_DATA_OFFSET = 104,
  AMDF_WINDOWS_XDNA_TRACE_PACKET_OVERHEAD = 112,
};

static amdf_status_t amdf_windows_xdna_trace_escape(
    amdf_xdna_umd_device_t* device, void* packet, uint32_t byte_length) {
  if (device->kmt->escape == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  D3DKMT_ESCAPE request = {0};
  request.hAdapter = device->adapter;
  request.hDevice = device->device;
  request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
  request.pPrivateDriverData = packet;
  request.PrivateDriverDataSize = byte_length;
  const NTSTATUS status = device->kmt->escape(&request);
  // STATUS_NOT_SUPPORTED is native service absence, not a permission failure.
  return (uint32_t)status == UINT32_C(0xc00000bb)
             ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
             : amdf_kmt_make_status(status);
}

static amdf_status_t amdf_windows_xdna_trace_state(
    amdf_xdna_umd_device_t* device, uint32_t write, uint32_t enabled,
    uint32_t categories, amdf_xdna_firmware_trace_info_t* out_info) {
  // The input block contains a four-byte query/configure selector. The second
  // block is an enabled byte, three padding bytes, category mask and version.
  uint32_t packet[28] = {
      [0] = 21, [1] = 96,       [4] = 4,           [6] = 1,
      [8] = 44, [12] = write,   [15] = 12,         [17] = 1,
      [21] = 1, [23] = enabled, [24] = categories,
  };
  const amdf_status_t status =
      amdf_windows_xdna_trace_escape(device, packet, sizeof(packet));
  if (amdf_status_is_ok(status) && out_info != NULL) {
    const uint32_t active = packet[23] & 255u;
    if (active > 1) {
      return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
    }
    *out_info = (amdf_xdna_firmware_trace_info_t){
        .enabled = active,
        .categories = active ? packet[24] : 0,
        .format_version = active ? packet[25] : 0,
        .buffer_alignment = 8,
        .minimum_buffer_byte_length =
            AMDF_WINDOWS_XDNA_TRACE_PACKET_OVERHEAD + 32,
        .recommended_buffer_byte_length =
            AMDF_WINDOWS_XDNA_TRACE_PACKET_OVERHEAD + 4 * 1024 * 1024,
    };
  }
  return status;
}

amdf_status_t amdf_xdna_umd_query_firmware_trace_info(
    amdf_xdna_umd_device_t* device, amdf_xdna_firmware_trace_info_t* out_info) {
  return amdf_windows_xdna_trace_state(device, 0, 0, 0, out_info);
}

amdf_status_t amdf_xdna_umd_context_query_firmware_trace_info(
    amdf_xdna_umd_context_t* context,
    amdf_xdna_context_firmware_trace_info_t* out_info) {
  // Native telemetry operation 11 returns an 8200-byte output block beginning
  // at byte 88, followed by eight serializer bytes. Its first two u64 fields
  // are miscellaneous telemetry and the RTOS task count. Each 248-byte task
  // contains the owning driver context selector at byte 208; the task ordinal
  // is its physical firmware ID. UINT64_MAX denotes an unowned slot.
  uint32_t packet[2074] = {
      [0] = 11, [1] = 8280, [6] = 1, [8] = 40, [14] = 8200, [16] = 1, [20] = 1,
  };
  const amdf_status_t status =
      amdf_windows_xdna_trace_escape(context->device, packet, sizeof(packet));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  const uint8_t* bytes = (const uint8_t*)packet;
  uint64_t task_count = 0;
  memcpy(&task_count, bytes + 96, sizeof(task_count));
  if (task_count > (sizeof(packet) - 8 - 104) / 248) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  uint32_t firmware_context_id = AMDF_XDNA_FIRMWARE_CONTEXT_ID_UNASSIGNED;
  for (uint32_t i = 0; i < task_count; ++i) {
    uint64_t selector = 0;
    memcpy(&selector, bytes + 104 + i * 248 + 208, sizeof(selector));
    if (selector == context->command_aperture_cookie) {
      // A native driver context can occupy only one firmware slot in this
      // snapshot. An ambiguous reply must not publish an arbitrary match.
      if (firmware_context_id != AMDF_XDNA_FIRMWARE_CONTEXT_ID_UNASSIGNED) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
      }
      firmware_context_id = i;
    }
  }
  *out_info = (amdf_xdna_context_firmware_trace_info_t){
      .firmware_context_id = firmware_context_id,
  };
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_configure_firmware_trace(
    amdf_xdna_umd_device_t* device, uint32_t enabled, uint32_t categories) {
  return amdf_windows_xdna_trace_state(device, 1, enabled,
                                       enabled ? categories : UINT32_MAX, NULL);
}

amdf_status_t amdf_xdna_umd_read_firmware_trace(
    amdf_xdna_umd_device_t* device, uint64_t position, void* buffer,
    uint32_t buffer_byte_length, amdf_observation_read_result_t* out_result) {
  if (buffer_byte_length < AMDF_WINDOWS_XDNA_TRACE_PACKET_OVERHEAD + 32) {
    return amdf_make_api_status(AMDF_STATUS_CODE_BUFFER_TOO_SMALL);
  }
  const uint32_t capacity =
      (buffer_byte_length - AMDF_WINDOWS_XDNA_TRACE_PACKET_OVERHEAD) & ~31u;
  uint32_t* packet = (uint32_t*)buffer;
  memset(packet, 0, AMDF_WINDOWS_XDNA_TRACE_DATA_OFFSET);
  memset((uint8_t*)buffer + AMDF_WINDOWS_XDNA_TRACE_DATA_OFFSET + capacity, 0,
         8);
  packet[0] = 22;
  packet[1] = capacity + 96;
  packet[6] = 1;
  packet[8] = 40;
  packet[14] = capacity + 16;
  packet[16] = 1;
  packet[20] = 1;
  const uint64_t native_capacity = capacity;
  memcpy(packet + 22, &native_capacity, sizeof(native_capacity));
  memcpy(packet + 24, &position, sizeof(position));
  const amdf_status_t status = amdf_windows_xdna_trace_escape(
      device, buffer, capacity + AMDF_WINDOWS_XDNA_TRACE_PACKET_OVERHEAD);
  if (!amdf_status_is_ok(status)) {
    return status;
  }

  uint64_t size = 0;
  uint64_t next = 0;
  memcpy(&size, packet + 22, sizeof(size));
  memcpy(&next, packet + 24, sizeof(next));
  if (size > capacity || size > next) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  amdf_observation_read_result_t result = {
      .next_position = next,
      .byte_offset = AMDF_WINDOWS_XDNA_TRACE_DATA_OFFSET,
      .byte_length = size,
  };
  if (next < position) {
    result.byte_length = 0;
    result.lost_byte_length = UINT64_MAX;
    result.flags =
        AMDF_OBSERVATION_READ_FLAG_GAP | AMDF_OBSERVATION_READ_FLAG_RESTART;
  } else {
    if (next - position < size) {
      return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
    }
    // The native offset is the producer limit, even when the copy filled its
    // destination before reaching that limit. Without the complete retained
    // window, truncation and overwritten history cannot be distinguished.
    if (size == capacity && next - position > size) {
      return amdf_make_api_status(AMDF_STATUS_CODE_BUFFER_TOO_SMALL);
    }
    result.lost_byte_length = next - position - size;
    if (result.lost_byte_length != 0) {
      result.flags = AMDF_OBSERVATION_READ_FLAG_GAP;
    }
  }
  *out_result = result;
  return AMDF_STATUS_OK;
}
