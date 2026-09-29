// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/memory_ordering.h"

#include "loom/ops/atomic.h"
#include "loom/ops/buffer/ops.h"
#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/legality.h"
#include "loom/target/arch/amdgpu/lower/memory.h"
#include "loom/target/arch/amdgpu/lower/memory_coherence.h"
#include "loom/target/arch/amdgpu/lower/system_memory.h"
#include "loom/target/arch/amdgpu/lower/topology.h"
#include "loom/target/arch/amdgpu/planning/wait_packets.h"

static bool loom_amdgpu_memory_ordering_available(
    const loom_low_descriptor_set_t* descriptor_set) {
  const loom_amdgpu_memory_coherence_rule_t* rule =
      loom_amdgpu_memory_coherence_rule(descriptor_set);
  if (!rule) {
    return false;
  }
  loom_amdgpu_wait_packet_selection_t selection = {0};
  return loom_amdgpu_system_memory_release_ordering_available(descriptor_set) &&
         loom_amdgpu_system_memory_acquire_ordering_available(descriptor_set) &&
         loom_amdgpu_wait_packet_try_select_counter_mask(
             descriptor_set, rule->local_wait_mask,
             /*target_count=*/0, &selection);
}

static bool loom_amdgpu_memory_ordering_scope_supported(uint8_t scope) {
  return scope == LOOM_ATOMIC_SCOPE_DEVICE || scope == LOOM_ATOMIC_SCOPE_SYSTEM;
}

loom_low_lower_visibility_model_t loom_amdgpu_memory_visibility_model(
    const loom_low_lower_context_t* context) {
  if (!loom_amdgpu_memory_ordering_available(
          loom_low_lower_context_descriptor_set(context))) {
    return (loom_low_lower_visibility_model_t){0};
  }
  return (loom_low_lower_visibility_model_t){
      .invocation_count = loom_amdgpu_target_wavefront_size(
          loom_low_lower_context_bundle(context)),
      .reuse_byte_limit = 4096,
      .reuse_count = 2,
  };
}

iree_string_view_t loom_amdgpu_atomic_memory_rejection_key(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source) {
  const bool is_workgroup =
      source->memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP;
  if (source->memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL &&
      !is_workgroup) {
    return IREE_SV("atomic.memory_space");
  }
  // Each observation uses one naturally aligned 32- or 64-bit memory packet.
  if (source->vector_lane_count != 1 ||
      (source->element_byte_count != 4 && source->element_byte_count != 8)) {
    return IREE_SV("atomic.value_type");
  }
  if (source->minimum_alignment < source->element_byte_count) {
    return IREE_SV("atomic.alignment");
  }
  if (is_workgroup ? source->atomic.scope != LOOM_ATOMIC_SCOPE_WORKGROUP
                   : !loom_amdgpu_memory_ordering_scope_supported(
                         source->atomic.scope)) {
    return IREE_SV("atomic.scope");
  }
  if (!loom_amdgpu_memory_ordering_available(descriptor_set)) {
    return IREE_SV("atomic.ordering");
  }
  return iree_string_view_empty();
}

static iree_status_t loom_amdgpu_emit_memory_wait(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint32_t counter_mask) {
  loom_amdgpu_wait_packet_selection_t selection = {0};
  const bool selected = loom_amdgpu_wait_packet_try_select_counter_mask(
      loom_low_lower_context_descriptor_set(context), counter_mask,
      /*target_count=*/0, &selection);
  IREE_ASSERT(selected);
  loom_amdgpu_explicit_packet_immediate_template_t
      immediates[LOOM_AMDGPU_WAIT_PACKET_SELECTION_IMMEDIATE_CAPACITY] = {0};
  for (iree_host_size_t i = 0; i < selection.immediate_count; ++i) {
    immediates[i] = (loom_amdgpu_explicit_packet_immediate_template_t){
        .name = selection.immediates[i].name,
        .value = selection.immediates[i].value,
    };
  }
  loom_amdgpu_explicit_packet_plan_t plan = {0};
  IREE_RETURN_IF_ERROR(loom_amdgpu_resolve_explicit_packet_row_plan(
      context, selection.descriptor, immediates, selection.immediate_count,
      &plan));
  return loom_amdgpu_emit_explicit_packet_plan(context, source_op, &plan);
}

static iree_status_t loom_amdgpu_emit_memory_release(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint8_t scope) {
  // Source fences and releases cover preceding reads and writes through every
  // address space, not just the global address holding a publication word.
  const loom_amdgpu_memory_coherence_rule_t* rule =
      loom_amdgpu_memory_coherence_rule(
          loom_low_lower_context_descriptor_set(context));
  if (scope == LOOM_ATOMIC_SCOPE_WORKGROUP) {
    for (uint8_t i = 0; i < rule->workgroup.release_wait_count; ++i) {
      IREE_RETURN_IF_ERROR(loom_amdgpu_emit_memory_wait(
          context, source_op, rule->workgroup.release_wait_masks[i]));
    }
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_emit_memory_wait(context, source_op, rule->local_wait_mask));
  return loom_amdgpu_system_memory_build_release_ordering_scoped(
      loom_low_lower_context_builder(context),
      loom_low_lower_context_descriptor_set(context),
      loom_amdgpu_memory_coherence_scope(scope), source_op->location);
}

iree_status_t loom_amdgpu_emit_memory_ordering_prefix(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_source_memory_access_plan_t* source) {
  if (source->operation_kind == LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_STORE &&
      source->atomic.ordering != LOOM_ATOMIC_ORDERING_RELAXED) {
    return loom_amdgpu_emit_memory_release(context, source_op,
                                           source->atomic.scope);
  }
  if (source->operation_kind == LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_LOAD &&
      source->atomic.ordering == LOOM_ATOMIC_ORDERING_SEQ_CST) {
    // Acquire alone does not prevent an earlier sequentially consistent store
    // from completing after this load. Drain prior accesses before the load.
    return loom_amdgpu_emit_memory_release(context, source_op,
                                           source->atomic.scope);
  }
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_emit_workgroup_memory_acquire(
    loom_low_lower_context_t* context, const loom_op_t* source_op) {
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_memory_wait(
      context, source_op, LOOM_AMDGPU_WAIT_COUNTER_MASK_LDS));
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(context);
  const loom_amdgpu_memory_coherence_rule_t* rule =
      loom_amdgpu_memory_coherence_rule(descriptor_set);
  if (!rule->workgroup.invalidate ||
      loom_low_lower_context_read_visibility_scope(context) !=
          LOOM_ATOMIC_SCOPE_THREAD) {
    return iree_ok_status();
  }
  loom_amdgpu_memory_coherence_attr_t
      attrs[LOOM_AMDGPU_MEMORY_COHERENCE_ATTR_CAPACITY];
  const uint8_t count = loom_amdgpu_memory_coherence_select_attrs(
      rule->cache_attrs[0], LOOM_CACHE_SCOPE_SE, attrs);
  loom_amdgpu_explicit_packet_immediate_template_t
      immediates[LOOM_AMDGPU_MEMORY_COHERENCE_ATTR_CAPACITY];
  for (uint8_t i = 0; i < count; ++i) {
    immediates[i] = (loom_amdgpu_explicit_packet_immediate_template_t){
        .name = attrs[i].name,
        .value = attrs[i].value,
    };
  }
  loom_amdgpu_explicit_packet_plan_t plan = {0};
  IREE_RETURN_IF_ERROR(loom_amdgpu_resolve_explicit_packet_row_plan(
      context,
      loom_amdgpu_lookup_descriptor_ref(descriptor_set,
                                        rule->workgroup.invalidate),
      immediates, count, &plan));
  return loom_amdgpu_emit_explicit_packet_plan(context, source_op, &plan);
}

iree_status_t loom_amdgpu_emit_memory_ordering_suffix(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_source_memory_access_plan_t* source) {
  if (source->operation_kind != LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_LOAD ||
      source->atomic.ordering == LOOM_ATOMIC_ORDERING_RELAXED) {
    return iree_ok_status();
  }
  if (source->memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP) {
    return loom_amdgpu_emit_workgroup_memory_acquire(context, source_op);
  }
  if (loom_low_lower_context_read_visibility_scope(context) !=
      LOOM_ATOMIC_SCOPE_THREAD) {
    return loom_amdgpu_system_memory_build_load_wait(
        loom_low_lower_context_builder(context),
        loom_low_lower_context_descriptor_set(context), source_op->location);
  }
  return loom_amdgpu_system_memory_build_acquire_ordering_scoped(
      loom_low_lower_context_builder(context),
      loom_low_lower_context_descriptor_set(context),
      loom_amdgpu_memory_coherence_scope(source->atomic.scope),
      source_op->location);
}

iree_status_t loom_amdgpu_select_memory_fence_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t* out_plan) {
  *out_plan = loom_low_lower_plan_empty();
  const uint8_t scope = loom_buffer_fence_scope(source_op);
  if (!loom_amdgpu_memory_ordering_scope_supported(scope) ||
      !loom_amdgpu_memory_ordering_available(
          loom_low_lower_context_descriptor_set(context))) {
    return iree_ok_status();
  }
  loom_amdgpu_memory_fence_plan_t* plan = NULL;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_allocate_plan_data(context, sizeof(*plan), (void**)&plan));
  *plan = (loom_amdgpu_memory_fence_plan_t){
      .ordering = loom_buffer_fence_ordering(source_op),
      .scope = scope,
  };
  *out_plan = loom_low_lower_plan_make(source_op->kind, plan);
  return iree_ok_status();
}

iree_status_t loom_amdgpu_lower_memory_fence(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_fence_plan_t* plan) {
  if (plan->ordering == LOOM_ATOMIC_ORDERING_ACQUIRE) {
    // An acquire fence may follow a no-return RMW or an LDS atomic. Complete
    // those observations without the writeback required by a release.
    const loom_amdgpu_memory_coherence_rule_t* rule =
        loom_amdgpu_memory_coherence_rule(
            loom_low_lower_context_descriptor_set(context));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_memory_wait(context, source_op,
                                                      rule->local_wait_mask));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_memory_wait(
        context, source_op, LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_STORE));
  } else {
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_emit_memory_release(context, source_op, plan->scope));
  }
  if (plan->ordering == LOOM_ATOMIC_ORDERING_RELEASE) {
    return iree_ok_status();
  }
  if (loom_low_lower_context_read_visibility_scope(context) !=
      LOOM_ATOMIC_SCOPE_THREAD) {
    return loom_amdgpu_system_memory_build_load_wait(
        loom_low_lower_context_builder(context),
        loom_low_lower_context_descriptor_set(context), source_op->location);
  }
  return loom_amdgpu_system_memory_build_acquire_ordering_scoped(
      loom_low_lower_context_builder(context),
      loom_low_lower_context_descriptor_set(context),
      loom_amdgpu_memory_coherence_scope(plan->scope), source_op->location);
}

iree_status_t loom_amdgpu_low_legality_verify_memory_fence(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled) {
  if (!loom_amdgpu_low_legality_context_is_amdgpu(context)) {
    return iree_ok_status();
  }
  *out_handled = true;
  if (!loom_amdgpu_memory_ordering_scope_supported(
          loom_buffer_fence_scope(op))) {
    return loom_amdgpu_low_legality_reject(context, op,
                                           IREE_SV("memory_fence.scope"));
  }
  if (!loom_amdgpu_memory_ordering_available(
          loom_target_low_legality_descriptor_set(context))) {
    return loom_amdgpu_low_legality_reject(context, op,
                                           IREE_SV("memory_fence.ordering"));
  }
  return iree_ok_status();
}
