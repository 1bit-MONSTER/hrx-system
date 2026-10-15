// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_FIRMWARE_TRACE_H_
#define AMDF_SRC_XDNA_UMD_FIRMWARE_TRACE_H_

#include "libamdf/src/xdna/umd/context.h"
#include "libamdf/src/xdna/umd/device.h"

#ifdef __cplusplus
extern "C" {
#endif

// Queries the native global recorder without enabling it.
amdf_status_t amdf_xdna_umd_query_firmware_trace_info(
    amdf_xdna_umd_device_t* device, amdf_xdna_firmware_trace_info_t* out_info);

// Queries the native context's current firmware slot without retaining it or
// certifying the lifetime of any earlier association.
amdf_status_t amdf_xdna_umd_context_query_firmware_trace_info(
    amdf_xdna_umd_context_t* context,
    amdf_xdna_context_firmware_trace_info_t* out_info);

// Mutates global native configuration under caller-arranged ownership.
amdf_status_t amdf_xdna_umd_configure_firmware_trace(
    amdf_xdna_umd_device_t* device, uint32_t enabled, uint32_t categories);

// Copies native records and reports their exact byte extent/progress. The
// provider validates native request geometry; the shared decoder validates
// externally produced record framing before publishing useful observations.
amdf_status_t amdf_xdna_umd_read_firmware_trace(
    amdf_xdna_umd_device_t* device, uint64_t position, void* buffer,
    uint32_t buffer_byte_length, amdf_observation_read_result_t* out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_UMD_FIRMWARE_TRACE_H_
