// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/clock.h"

#include <string.h>

#include "libamdf/src/xdna/umd/mcdm/device.h"

amdf_status_t amdf_xdna_umd_query_timer_clock_info(
    amdf_xdna_umd_device_t* device, amdf_observation_clock_info_t* out_info) {
  if (device->kmt->escape == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  // XRT clock-list query: operation 4, selector 1, 4096-byte text capacity.
  // The two serialized parameter blocks have 32-byte headers and eight-byte
  // trailers. Output starts with an eight-byte extent then decimal MHz lines.
  uint32_t packet[1054] = {
      [0] = 4,  [1] = 4200,  [4] = 8,     [6] = 1,  [8] = 48,
      [12] = 1, [13] = 4096, [16] = 4112, [18] = 1, [22] = 1,
  };
  D3DKMT_ESCAPE request = {0};
  request.hAdapter = device->adapter;
  request.hDevice = device->device;
  request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
  request.pPrivateDriverData = packet;
  request.PrivateDriverDataSize = sizeof(packet);
  const NTSTATUS native_status = device->kmt->escape(&request);
  if ((uint32_t)native_status == UINT32_C(0xc00000bb)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  const amdf_status_t status = amdf_kmt_make_status(native_status);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  const char* text = (const char*)(packet + 26);
  if (packet[24] != 4112 || packet[25] != 0 || memchr(text, 0, 4096) == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  // The first clock is the native-reported array rate. This format provides
  // no sample age or telemetry-validity field. Other lines describe separate
  // clocks and cannot replace it. KMT node/calibration frequencies belong to
  // different counters and are not a fallback for this source.
  uint32_t frequency_mhz = 0;
  const char* cursor = text;
  while (*cursor >= '0' && *cursor <= '9') {
    const uint32_t digit = (uint32_t)(*cursor++ - '0');
    if (frequency_mhz > (UINT32_MAX - digit) / 10) {
      return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
    }
    frequency_mhz = frequency_mhz * 10 + digit;
  }
  if (cursor == text || *cursor != '\n') {
    return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  if (frequency_mhz == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  *out_info = (amdf_observation_clock_info_t){
      .frequency_hz = (uint64_t)frequency_mhz * 1000000,
      .counter_bit_count = 64,
  };
  return AMDF_STATUS_OK;
}
