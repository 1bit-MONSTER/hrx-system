// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Wasm dynamic vector-slice lowering costs and limits.

#ifndef LOOM_TARGET_EMIT_WASM_LOWER_VECTOR_DYNAMIC_SLICE_H_
#define LOOM_TARGET_EMIT_WASM_LOWER_VECTOR_DYNAMIC_SLICE_H_

#include "loom/ir/ir.h"
#include "loom/target/emit/wasm/lower/vector_carrier.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  // Maximum Low operations emitted by one dynamic tuple slice.
  LOOM_WASM_DYNAMIC_SLICE_MAX_OPERATION_COUNT = 128,
};

typedef struct loom_value_fact_table_t loom_value_fact_table_t;

// Matches a rank-one dynamic slice whose source has a retained authored vector
// partition and returns its source and result values.
bool loom_wasm_dynamic_slice_values(const loom_module_t* module,
                                    const loom_value_fact_table_t* fact_table,
                                    const loom_op_t* source_op,
                                    loom_value_id_t* out_source,
                                    loom_value_id_t* out_result);

// Returns the number of packet-boundary thresholds selected at runtime.
uint32_t loom_wasm_dynamic_slice_comparison_count(
    loom_wasm_vector_carrier_t source_carrier,
    loom_wasm_vector_carrier_t result_carrier);

// Returns the complete number of descriptor and tuple operations emitted for
// a dynamic slice between the two compatible carriers.
uint32_t loom_wasm_dynamic_slice_operation_count(
    loom_wasm_vector_carrier_t source_carrier,
    loom_wasm_vector_carrier_t result_carrier);

// Returns whether the dynamic slice stays within the per-operation IR limit.
bool loom_wasm_dynamic_slice_operation_count_is_bounded(
    loom_wasm_vector_carrier_t source_carrier,
    loom_wasm_vector_carrier_t result_carrier);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_WASM_LOWER_VECTOR_DYNAMIC_SLICE_H_
