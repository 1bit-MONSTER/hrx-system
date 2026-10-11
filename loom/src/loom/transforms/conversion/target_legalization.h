// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_CONVERSION_TARGET_LEGALIZATION_H_
#define LOOM_TRANSFORMS_CONVERSION_TARGET_LEGALIZATION_H_

#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns reference decompositions shared by scalar and vector conversions.
// Compose after target providers so directly supported conversions retain their
// native lowering. Intermediate formats preserve the original rounding result.
const loom_target_legalizer_provider_t*
loom_conversion_target_legalizer_provider(void);

// Rewrites an unsupported F16/FP8-to-I64 conversion through exact F32 and
// I32/U32 intermediates. Every source value in the conversion's defined domain
// fits the 32-bit integer stage. The caller establishes the source and result
// domains and that the selected target has no preferable direct conversion.
iree_status_t loom_conversion_legalize_narrow_float_to_i64(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CONVERSION_TARGET_LEGALIZATION_H_
