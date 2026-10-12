// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU hardware-transposing matrix-fragment memory packets.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_FRAGMENT_MEMORY_TRANSPOSE_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_FRAGMENT_MEMORY_TRANSPOSE_H_

#include "loom/target/arch/amdgpu/lower/fragment_memory/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// A complete hardware-transposing replacement for an ordinary fragment load.
// Selection proves the hardware lane/register exchange against the generated
// semantic coordinate projection and retains only directly emitted plan data.
typedef struct loom_amdgpu_fragment_memory_transpose_choice_t {
  // Compiled issuer-lane and packet address coefficients.
  loom_amdgpu_fragment_memory_address_layout_t address_layout;
  // Hardware-transposing packets in increasing fragment-register order.
  loom_amdgpu_fragment_memory_packet_plan_t
      packets[LOOM_AMDGPU_MAX_MATRIX_FRAGMENT_32BIT_REGISTERS];
  // Number of populated packet plans.
  uint16_t packet_count;
  // Whether every selected packet consumes its address source at issue.
  bool packet_address_sources_consumed_at_issue;
} loom_amdgpu_fragment_memory_transpose_choice_t;

// Selects an exact native transpose-load choice for |plan|. Source preparation
// has established the logical access and fragment storage, but packetization
// and address-range selection may not have run yet. Returns false when the
// target, memory layout, or fragment projection does not exactly match a
// supported hardware transpose.
bool loom_amdgpu_fragment_memory_select_transpose_load(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_matrix_fragment_layout_t* layout,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    loom_amdgpu_fragment_memory_transpose_choice_t* out_choice);

// Applies a previously selected native transpose-load choice to |plan|.
void loom_amdgpu_fragment_memory_apply_transpose_load(
    const loom_amdgpu_fragment_memory_transpose_choice_t* choice,
    loom_amdgpu_fragment_memory_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_FRAGMENT_MEMORY_TRANSPOSE_H_
