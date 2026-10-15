// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_FIRMWARE_TRACE_FORMAT_H_
#define AMDF_SRC_XDNA_FIRMWARE_TRACE_FORMAT_H_

#include "amdf/xdna_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

// Validates format-2 native framing against exact byte positions, then compacts
// events in place. Invalid external records discard the entire copied batch
// with explicit loss; no record from that batch is published as valid timing.
// Native providers establish the result's storage bounds; the service checks
// that next_position identifies a complete frame boundary before this call.
void amdf_xdna_firmware_trace_decode(uint64_t requested_position, void* buffer,
                                     amdf_observation_read_result_t* result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_FIRMWARE_TRACE_FORMAT_H_
