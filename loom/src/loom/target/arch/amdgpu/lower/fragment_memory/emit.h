// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU low emission for selected vector fragment memory plans.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_FRAGMENT_MEMORY_EMIT_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_FRAGMENT_MEMORY_EMIT_H_

#include "loom/target/arch/amdgpu/lower/fragment_memory/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Lowers a source vector.fragment.load op to lane-owned memory packets.
iree_status_t loom_amdgpu_lower_vector_fragment_load(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan);

// Lowers a source vector.fragment.store op to lane-owned memory packets.
iree_status_t loom_amdgpu_lower_vector_fragment_store(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan);

// Materializes the exact subgroup mask whose lanes publish a selected
// cross-lane packed-B16 fragment store.
iree_status_t loom_amdgpu_emit_fragment_memory_publishing_lane_mask(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    const loom_amdgpu_matrix_fragment_lane_ids_t* lane_ids,
    loom_type_t vgpr_type, loom_type_t mask_type,
    loom_value_id_t* out_low_publishing_lane_mask);

// Marks the physical source values needed by a selected AMDGPU fragment memory
// plan.
void loom_amdgpu_mark_fragment_memory_plan_storage_demands(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan);

// Marks only the source values needed to materialize a selected fragment
// memory plan's physical addresses.
void loom_amdgpu_mark_fragment_memory_address_storage_demands(
    loom_low_lower_context_t* context,
    const loom_amdgpu_fragment_memory_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_FRAGMENT_MEMORY_EMIT_H_
