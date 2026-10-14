// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Source-origin proofs retained for target instruction memory effects.

#ifndef LOOM_CODEGEN_LOW_LOWER_MEMORY_EFFECTS_H_
#define LOOM_CODEGEN_LOW_LOWER_MEMORY_EFFECTS_H_

#include "loom/codegen/low/descriptors.h"
#include "loom/codegen/low/source_memory_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_context_t loom_low_lower_context_t;
typedef struct loom_low_lower_memory_origin_t loom_low_lower_memory_origin_t;

// Physical instruction geometry projected over canonical source access facts.
typedef struct loom_low_lower_memory_packet_geometry_t {
  // Static byte offset selected for the instruction.
  int64_t static_byte_offset;
  // Number of bytes in each logical vector element.
  uint32_t element_byte_count;
  // Number of logical vector elements moved by the instruction.
  uint32_t vector_lane_count;
  // Byte distance between consecutive logical vector elements.
  int64_t vector_lane_byte_stride;
} loom_low_lower_memory_packet_geometry_t;

// Returns the canonical instruction geometry represented by |source_plan|.
static inline loom_low_lower_memory_packet_geometry_t
loom_low_lower_memory_packet_geometry_from_source(
    const loom_low_source_memory_access_plan_t* source_plan) {
  return (loom_low_lower_memory_packet_geometry_t){
      .static_byte_offset = source_plan->static_byte_offset,
      .element_byte_count = source_plan->element_byte_count,
      .vector_lane_count = source_plan->vector_lane_count,
      .vector_lane_byte_stride = source_plan->vector_lane_byte_stride,
  };
}

// Captures the canonical access's dynamic origin independently of its physical
// instruction geometry. Only workgroup-uniform terms may retain correlations;
// all other terms contribute to a conservative participant-varying envelope.
// The result owns its expression storage in |arena|. Static accesses and roots
// without a common workgroup address return NULL without allocating.
iree_status_t loom_low_lower_memory_origin_plan(
    loom_symbolic_expr_context_t* expressions,
    const loom_low_source_memory_access_plan_t* source_plan,
    iree_arena_allocator_t* arena,
    const loom_low_lower_memory_origin_t** out_origin);

// Projects selected instruction coordinates onto the retained origin without
// source analysis. Dynamic terms are unchanged from the canonical access;
// physical allocation offsets and instruction geometry may differ. Returns
// false when the available proof cannot bound the access. The caller supplies
// the evaluation namespace; the origin keeps captured value identities.
bool loom_low_lower_memory_packet_interval(
    const loom_low_lower_memory_origin_t* origin,
    const loom_low_source_memory_access_plan_t* source_plan,
    const loom_low_lower_memory_packet_geometry_t* geometry,
    loom_value_facts_t additional_offset,
    loom_low_memory_relative_interval_t* out_interval,
    int64_t* out_lane_byte_count);

// Retains a producer-owned footprint for one exact descriptor effect. The
// result owns a deep copy in the module arena; source analysis may then expire.
iree_status_t loom_low_lower_record_memory_effect(
    loom_low_lower_context_t* context, const loom_op_t* low_op,
    uint16_t effect_ordinal, const loom_low_memory_access_summary_t* summary);

// Records an instruction from retained source-memory selection data. The
// caller supplies selected geometry over the origin's canonical dynamic terms;
// additional_offset bounds runtime instruction coordinates.
iree_status_t loom_low_lower_record_memory_packet(
    loom_low_lower_context_t* context, const loom_op_t* low_op,
    const loom_low_descriptor_t* descriptor,
    const loom_low_lower_memory_origin_t* origin,
    const loom_low_source_memory_access_plan_t* source_plan,
    const loom_low_lower_memory_packet_geometry_t* geometry,
    loom_value_facts_t additional_offset);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_MEMORY_EFFECTS_H_
