// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Coherence domains and completion/visibility packets for native atomic
// updates. Selection retains the full recipe; emission consumes it without
// rediscovery.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_ATOMIC_ORDERING_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_ATOMIC_ORDERING_H_

#include "loom/target/arch/amdgpu/lower/memory_coherence.h"
#include "loom/target/arch/amdgpu/lower/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// An ordering packet before descriptor resolution and attribute interning.
typedef struct loom_amdgpu_atomic_explicit_packet_selection_t {
  // Stable descriptor ref selected for this explicit packet.
  loom_amdgpu_descriptor_ref_t descriptor_ref;
  // Immediate rows emitted on the descriptor.
  loom_amdgpu_explicit_packet_immediate_template_t
      immediates[LOOM_AMDGPU_EXPLICIT_PACKET_IMMEDIATE_CAPACITY];
  // Number of populated immediate rows.
  iree_host_size_t immediate_count;
} loom_amdgpu_atomic_explicit_packet_selection_t;

// Completion and visibility around one RMW, reduction, or compare-exchange.
typedef struct loom_amdgpu_atomic_ordering_selection_t {
  // Completion and writeback packets emitted before the atomic packet.
  loom_amdgpu_atomic_explicit_packet_selection_t
      pre_atomic_packets[LOOM_AMDGPU_ATOMIC_PREFIX_CAPACITY];
  // Number of populated pre-atomic packets.
  iree_host_size_t pre_atomic_packet_count;
  // Explicit waits emitted after the atomic packet.
  loom_amdgpu_atomic_explicit_packet_selection_t
      post_atomic_waits[LOOM_AMDGPU_ATOMIC_WAIT_CAPACITY];
  // Number of populated post-atomic wait packets.
  iree_host_size_t post_atomic_wait_count;
  // Cache invalidation and its completion after the atomic packet.
  loom_amdgpu_atomic_explicit_packet_selection_t
      post_atomic_visibility_packets[LOOM_AMDGPU_ATOMIC_VISIBILITY_CAPACITY];
  // Number of populated post-atomic visibility packets.
  iree_host_size_t post_atomic_visibility_packet_count;
} loom_amdgpu_atomic_ordering_selection_t;

// Queries coherence-domain support for the physical packet's value type,
// independently of the arithmetic opcode or logical subword footprint.
// System updates require naturally aligned, system-atomic backing supplied by
// the caller; host accessibility alone does not establish that contract.
bool loom_amdgpu_atomic_scope_supported(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source, loom_type_t value_type);

// Queries target support for the update's success and failure ordering.
bool loom_amdgpu_atomic_orderings_supported(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source);

// Selects the atomic packet's single coherence field from the source scope.
// An empty name indicates that the descriptor needs no coherence attribute.
loom_amdgpu_memory_coherence_attr_t loom_amdgpu_atomic_select_packet_attr(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source);

// Retains every completion and visibility packet required by an update.
// Returns false when a required instruction is unavailable on the target.
bool loom_amdgpu_atomic_select_ordering(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source,
    loom_amdgpu_atomic_operation_kind_t operation_kind,
    loom_amdgpu_atomic_ordering_selection_t* ordering);

// Resolves selected descriptors and immediates once for later emission.
iree_status_t loom_amdgpu_atomic_resolve_ordering_selection(
    loom_low_lower_context_t* context,
    const loom_amdgpu_atomic_ordering_selection_t* selection,
    loom_amdgpu_atomic_ordering_plan_t* out_plan);

// Completes accesses ordered before the atomic publication.
iree_status_t loom_amdgpu_emit_atomic_pre_ordering(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_atomic_ordering_plan_t* ordering);

// Completes the observation and establishes visibility for following accesses.
iree_status_t loom_amdgpu_emit_atomic_post_ordering(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_atomic_ordering_plan_t* ordering);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_ATOMIC_ORDERING_H_
