// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_FIRMWARE_TRACE_H_
#define AMDF_SRC_XDNA_FIRMWARE_TRACE_H_

#include "amdf/xdna.h"

#ifdef __cplusplus
extern "C" {
#endif

amdf_status_t AMDF_CALL amdf_xdna_device_query_firmware_trace_info(
    amdf_device_t* device, amdf_xdna_firmware_trace_info_t* out_info);
amdf_status_t AMDF_CALL amdf_xdna_device_configure_firmware_trace(
    amdf_device_t* device, uint32_t enabled,
    amdf_xdna_firmware_trace_categories_t categories);
amdf_status_t AMDF_CALL amdf_xdna_device_read_firmware_trace(
    amdf_device_t* device, uint32_t format_version, uint64_t position,
    void* buffer, uint32_t buffer_byte_length,
    amdf_observation_read_result_t* out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_FIRMWARE_TRACE_H_
