// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/firmware_trace_format.h"

#include <string.h>

static void amdf_xdna_firmware_trace_discard(
    uint64_t requested_position, amdf_observation_read_result_t* result) {
  result->flags |=
      AMDF_OBSERVATION_READ_FLAG_GAP | AMDF_OBSERVATION_READ_FLAG_DISCARDED;
  result->lost_byte_length = result->next_position - requested_position;
  result->byte_length = 0;
}

void amdf_xdna_firmware_trace_decode(uint64_t requested_position, void* buffer,
                                     amdf_observation_read_result_t* result) {
  if (result->flags & AMDF_OBSERVATION_READ_FLAG_RESTART) {
    return;
  }
  if (result->byte_length % 32 != 0) {
    amdf_xdna_firmware_trace_discard(requested_position, result);
    return;
  }
  const uint64_t begin = result->next_position - result->byte_length;
  uint8_t* records = (uint8_t*)buffer + result->byte_offset;
  const uint64_t count = result->byte_length / 32;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t words[4];
    memcpy(words, records + i * 32, sizeof(words));
    const uint64_t sequence = ((begin / 32 + i) % 65535) + 1;
    if (words[0] != (UINT64_C(0x02ca) | (sequence << 16)) ||
        words[3] != ((UINT64_C(0xba02) << 48) | (sequence << 32))) {
      amdf_xdna_firmware_trace_discard(requested_position, result);
      return;
    }
  }
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t words[4];
    memcpy(words, records + i * 32, sizeof(words));
    const amdf_xdna_firmware_trace_event_t event = {
        .timestamp = words[1],
        .arguments = words[2] & UINT64_C(0x0000ffffffffffff),
        .event_id = (uint32_t)(words[2] >> 48),
        .sequence = (uint32_t)(words[0] >> 16),
    };
    memcpy(records + i * sizeof(event), &event, sizeof(event));
  }
  result->byte_length = count * sizeof(amdf_xdna_firmware_trace_event_t);
}
