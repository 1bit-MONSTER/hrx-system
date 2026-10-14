// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_RESULT_BUFFER_H_
#define AMDF_SRC_XDNA_UMD_RESULT_BUFFER_H_

#include "libamdf/src/xdna/umd/context.h"
#include "libamdf/src/xdna/umd/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

// Context-native controls on an externally serialized finite destination.
typedef enum amdf_xdna_result_buffer_operation_e {
  AMDF_XDNA_RESULT_BUFFER_OPERATION_ATTACH,
  AMDF_XDNA_RESULT_BUFFER_OPERATION_COLLECT,
  AMDF_XDNA_RESULT_BUFFER_OPERATION_DETACH,
} amdf_xdna_result_buffer_operation_t;

// Describes immutable native geometry without making a native call. A provider
// protocol lacking the result service returns UNSUPPORTED with no output.
amdf_status_t amdf_xdna_umd_context_query_result_buffer_info(
    amdf_xdna_umd_context_t* context, amdf_xdna_result_buffer_info_t* out_info);

// Executes one native control on validated context-private memory. The caller
// has established permissions, logical bounds and geometry and has retired all
// writers. The provider retains neither memory nor attachment metadata.
amdf_status_t amdf_xdna_umd_context_result_buffer(
    amdf_xdna_umd_context_t* context,
    amdf_xdna_result_buffer_operation_t operation,
    amdf_xdna_umd_memory_t* memory, uint64_t byte_offset, uint64_t byte_length);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_UMD_RESULT_BUFFER_H_
