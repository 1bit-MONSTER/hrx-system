// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_ATOMIC_SUBWORD_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_ATOMIC_SUBWORD_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/target/arch/amdgpu/lower/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits a strong byte/halfword compare-exchange using a word CAS. The selected
// plan retains the logical footprint; the containing physical word stays in
// the mapped page or workgroup allocation. Neighbor bits survive updates, and
// completed lanes retain their old payload while other lanes retry.
iree_status_t loom_amdgpu_emit_subword_cmpxchg(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_atomic_plan_t* plan, loom_value_id_t address,
    loom_value_id_t expected, loom_value_id_t replacement,
    loom_named_attr_slice_t packet_attrs, loom_value_id_t* out_old);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_ATOMIC_SUBWORD_H_
