// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Aggregate reconstruction for vector-to-scalar lowering.

#ifndef LOOM_TRANSFORMS_VECTOR_TO_SCALAR_AGGREGATES_H_
#define LOOM_TRANSFORMS_VECTOR_TO_SCALAR_AGGREGATES_H_

#include "loom/transforms/vector/to_scalar_lanes.h"

#ifdef __cplusplus
extern "C" {
#endif

iree_status_t loom_vector_to_scalar_lower_splat(
    loom_vector_to_scalar_state_t* state, loom_value_id_t* out_replacement);

iree_status_t loom_vector_to_scalar_lower_aggregate(
    loom_vector_to_scalar_state_t* state, loom_value_id_t* out_replacement);

// Reconstructs all same-shaped results of one mechanical scalar lane program.
// Dynamic results share one loop nest and carry one aggregate per result.
iree_status_t loom_vector_to_scalar_lower_aggregates(
    loom_vector_to_scalar_state_t* state, uint16_t result_count,
    loom_value_id_t* out_replacements);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_TO_SCALAR_AGGREGATES_H_
