// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/source_memory.h"

#include <string.h>

#include "loom/codegen/low/lower/context.h"
#include "loom/codegen/low/lower/realization.h"
#include "loom/ir/structural_hash.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/view/ops.h"
#include "loom/util/fact_cfg.h"

static_assert(LOOM_LOW_SOURCE_MEMORY_DYNAMIC_TERM_CAPACITY <= 16,
              "address components must fit their canonical term mask");

typedef struct loom_low_lower_memory_component_candidate_t {
  // Access that published this exact source realization.
  loom_low_lower_source_memory_record_t* record;
  // Realization borrowed from a function-owned canonical access record.
  const loom_low_source_memory_dynamic_term_t* term;
  // Canonical members in the publishing record.
  uint16_t term_mask;
} loom_low_lower_memory_component_candidate_t;

typedef struct loom_low_lower_memory_component_entry_t {
  // Canonical terms defining this interned byte sum.
  const loom_low_source_memory_access_plan_t* key_access;
  // Members of key_access's dynamic terms.
  uint16_t key_mask;
  // Retained structural hash used during table growth.
  uint32_t hash;
  // Most recently published in-scope realization of this sum.
  loom_low_lower_memory_component_candidate_t* candidate;
  // Next structural-key collision in the bucket.
  struct loom_low_lower_memory_component_entry_t* next;
  // Next publication retired with the same dominance scope.
  struct loom_low_lower_memory_component_entry_t* scope_next;
} loom_low_lower_memory_component_entry_t;

typedef struct loom_low_lower_memory_component_scope_t {
  // Block whose operations and dominated descendants share this scope.
  const loom_block_t* block;
  // Publications owned by this scope, excluding inherited candidates.
  loom_low_lower_memory_component_entry_t* first;
  // Enclosing structured or CFG dominance scope.
  struct loom_low_lower_memory_component_scope_t* parent;
} loom_low_lower_memory_component_scope_t;

struct loom_low_lower_source_memory_builder_t {
  // Last function-owned record appended by the shared source walk.
  loom_low_lower_source_memory_record_t* last;
  // Scratch lifetime shared with the existing source preparation walk.
  iree_arena_allocator_t* arena;
  // Borrowed indexed dominance for the function's flat CFG, if present.
  const loom_value_fact_cfg_region_t* cfg;
  // Active scopes maintained by block entry and structured region exit.
  loom_low_lower_memory_component_scope_t* scope;
  // Interned canonical term sets, allocated in planning scratch.
  loom_low_lower_memory_component_entry_t** buckets;
  // Power-of-two bucket count, or zero before the first realization.
  iree_host_size_t bucket_count;
  // Number of distinct canonical term sets in buckets.
  iree_host_size_t entry_count;
};

static void loom_low_lower_memory_component_leave_scope(
    loom_low_lower_source_memory_builder_t* builder) {
  loom_low_lower_memory_component_scope_t* scope = builder->scope;
  for (loom_low_lower_memory_component_entry_t* entry = scope->first; entry;
       entry = entry->scope_next) {
    entry->candidate = NULL;
  }
  builder->scope = scope->parent;
}

iree_status_t loom_low_lower_source_memory_enter_block(
    loom_low_lower_source_memory_builder_t* builder,
    const loom_block_t* block) {
  // The flat CFG is visited in dominator preorder. Retire completed subtrees
  // once as that order advances; nested structured regions remain bracketed
  // by the source visitor. No memory access needs to rediscover its ancestry.
  while (builder->scope &&
         builder->scope->block->parent_region == block->parent_region &&
         !loom_cfg_dominance_block_dominates(
             &builder->cfg->dominance, builder->scope->block->region_index,
             block->region_index)) {
    loom_low_lower_memory_component_leave_scope(builder);
  }
  loom_low_lower_memory_component_scope_t* scope = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(builder->arena, sizeof(*scope), (void**)&scope));
  *scope = (loom_low_lower_memory_component_scope_t){.block = block,
                                                     .parent = builder->scope};
  builder->scope = scope;
  return iree_ok_status();
}

void loom_low_lower_source_memory_leave_region(
    loom_low_lower_source_memory_builder_t* builder,
    const loom_region_t* region) {
  while (builder->scope && builder->scope->block->parent_region == region) {
    loom_low_lower_memory_component_leave_scope(builder);
  }
}

static uint16_t loom_low_lower_memory_term_range(uint8_t first, uint8_t count) {
  return (uint16_t)(((1u << count) - 1u) << first);
}

static uint32_t loom_low_lower_memory_term_hash(
    const loom_low_source_memory_dynamic_term_t* term) {
  uint32_t hash = loom_structural_hash_initialize();
  hash = loom_structural_hash_mix_u32(hash, term->source);
  hash = loom_structural_hash_mix_u32(
      hash, term->source == LOOM_LOW_SOURCE_MEMORY_DYNAMIC_INDEX_SOURCE_VALUE
                ? term->index
                : (uint32_t)term->dimension);
  hash = loom_structural_hash_mix_u64(hash, (uint64_t)term->byte_stride);
  hash = loom_structural_hash_mix_u8(hash, term->stride_value_count);
  for (uint8_t i = 0; i < term->stride_value_count; ++i) {
    hash = loom_structural_hash_mix_u32(hash, term->stride_values[i]);
  }
  return loom_structural_hash_finalize(hash);
}

static bool loom_low_lower_memory_terms_equal(
    const loom_low_source_memory_dynamic_term_t* lhs,
    const loom_low_source_memory_dynamic_term_t* rhs) {
  if (lhs->source != rhs->source || lhs->byte_stride != rhs->byte_stride ||
      lhs->stride_value_count != rhs->stride_value_count) {
    return false;
  }
  if (lhs->source == LOOM_LOW_SOURCE_MEMORY_DYNAMIC_INDEX_SOURCE_VALUE
          ? lhs->index != rhs->index
          : lhs->dimension != rhs->dimension) {
    return false;
  }
  return memcmp(lhs->stride_values, rhs->stride_values,
                lhs->stride_value_count * sizeof(*lhs->stride_values)) == 0;
}

static uint32_t loom_low_lower_memory_component_hash(
    const loom_low_source_memory_access_plan_t* access, uint16_t mask) {
  // A set may be noncontiguous or have a different canonical ordering in a
  // different access. Equality below compares the complete term multiset.
  uint32_t hash = 0;
  uint32_t count = 0;
  for (uint8_t i = 0; i < access->dynamic_term_count; ++i) {
    if (mask & (1u << i)) {
      hash += loom_low_lower_memory_term_hash(&access->dynamic_terms[i]);
      ++count;
    }
  }
  return loom_structural_hash_finalize(
      loom_structural_hash_mix_u32(hash, count));
}

static bool loom_low_lower_memory_components_equal(
    const loom_low_source_memory_access_plan_t* lhs, uint16_t lhs_mask,
    const loom_low_source_memory_access_plan_t* rhs, uint16_t rhs_mask) {
  for (uint8_t i = 0; i < lhs->dynamic_term_count; ++i) {
    if (!(lhs_mask & (1u << i))) {
      continue;
    }
    bool found = false;
    for (uint8_t j = 0; j < rhs->dynamic_term_count; ++j) {
      if ((rhs_mask & (1u << j)) &&
          loom_low_lower_memory_terms_equal(&lhs->dynamic_terms[i],
                                            &rhs->dynamic_terms[j])) {
        rhs_mask &= (uint16_t)~(1u << j);
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return rhs_mask == 0;
}

static loom_low_lower_memory_component_entry_t*
loom_low_lower_memory_component_find(
    loom_low_lower_source_memory_builder_t* builder,
    const loom_low_source_memory_access_plan_t* access, uint16_t mask,
    uint32_t hash) {
  if (builder->bucket_count == 0) {
    return NULL;
  }
  for (loom_low_lower_memory_component_entry_t* entry =
           builder->buckets[hash & (builder->bucket_count - 1)];
       entry != NULL; entry = entry->next) {
    if (entry->hash == hash &&
        loom_low_lower_memory_components_equal(entry->key_access,
                                               entry->key_mask, access, mask)) {
      return entry;
    }
  }
  return NULL;
}

static iree_status_t loom_low_lower_memory_components_grow(
    loom_low_lower_source_memory_builder_t* builder) {
  const iree_host_size_t bucket_count =
      builder->bucket_count ? builder->bucket_count * 2 : 32;
  loom_low_lower_memory_component_entry_t** buckets = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      builder->arena, bucket_count, sizeof(*buckets), (void**)&buckets));
  memset(buckets, 0, bucket_count * sizeof(*buckets));
  for (iree_host_size_t i = 0; i < builder->bucket_count; ++i) {
    loom_low_lower_memory_component_entry_t* entry = builder->buckets[i];
    while (entry != NULL) {
      loom_low_lower_memory_component_entry_t* next = entry->next;
      const iree_host_size_t bucket = entry->hash & (bucket_count - 1);
      entry->next = buckets[bucket];
      buckets[bucket] = entry;
      entry = next;
    }
  }
  builder->buckets = buckets;
  builder->bucket_count = bucket_count;
  return iree_ok_status();
}

static void loom_low_lower_memory_component_select(
    loom_low_lower_source_memory_builder_t* builder,
    loom_low_lower_source_memory_record_t* record, uint16_t mask) {
  if (mask == 0 || (mask & (mask - 1)) == 0) {
    return;
  }
  loom_low_lower_memory_component_entry_t* entry =
      loom_low_lower_memory_component_find(
          builder, &record->access, mask,
          loom_low_lower_memory_component_hash(&record->access, mask));
  if (entry == NULL) {
    return;
  }
  if (entry->candidate != NULL) {
    record->access.retained_component =
        (loom_low_source_memory_dynamic_component_t){
            .term = entry->candidate->term,
            .term_mask = mask,
        };
    // Once another access demands the SSA sum, its original access should
    // consume it too. This matters for coordinate materializers that do not
    // opportunistically reuse local source expressions during emission.
    loom_low_source_memory_dynamic_component_t* producer_component =
        &entry->candidate->record->access.retained_component;
    if (producer_component->term == NULL) {
      *producer_component = (loom_low_source_memory_dynamic_component_t){
          .term = entry->candidate->term,
          .term_mask = entry->candidate->term_mask,
      };
    }
  }
}

static bool loom_low_lower_memory_value_is_loop_invariant(
    const loom_module_t* module, const loom_cfg_loop_nest_t* loops,
    uint16_t loop_index, loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(module, value_id);
  const loom_block_t* block = loom_value_is_block_arg(value)
                                  ? loom_value_def_block(value)
                                  : loom_value_def_op(value)->parent_block;
  return block->parent_region != loops->graph->region ||
         !loom_cfg_loop_nest_contains(loops, loop_index, block->region_index);
}

static uint16_t loom_low_lower_memory_invariant_terms(
    const loom_low_lower_context_t* context,
    const loom_low_lower_source_memory_record_t* record) {
  const loom_value_fact_cfg_region_t* cfg = loom_low_lower_context_cfg(context);
  const loom_block_t* block = record->source_op->parent_block;
  if (cfg == NULL || block->parent_region != cfg->graph.region) {
    return 0;
  }
  const uint16_t loop_index =
      loom_cfg_loop_nest_innermost(&cfg->loops, block->region_index);
  if (loop_index == LOOM_CFG_LOOP_NEST_NONE) {
    return 0;
  }
  uint16_t mask = 0;
  for (uint8_t i = 0; i < record->access.dynamic_term_count; ++i) {
    const loom_low_source_memory_dynamic_term_t* term =
        &record->access.dynamic_terms[i];
    bool invariant =
        term->source != LOOM_LOW_SOURCE_MEMORY_DYNAMIC_INDEX_SOURCE_VALUE ||
        loom_low_lower_memory_value_is_loop_invariant(
            context->module, &cfg->loops, loop_index, term->index);
    for (uint8_t j = 0; invariant && j < term->stride_value_count; ++j) {
      invariant = loom_low_lower_memory_value_is_loop_invariant(
          context->module, &cfg->loops, loop_index, term->stride_values[j]);
    }
    if (invariant) {
      mask |= (uint16_t)(1u << i);
    }
  }
  return mask;
}

static iree_status_t loom_low_lower_memory_components_publish(
    loom_low_lower_source_memory_builder_t* builder,
    loom_low_lower_source_memory_record_t* record) {
  for (uint8_t i = 0; i < record->access.dynamic_realization_count; ++i) {
    const loom_low_source_memory_dynamic_realization_t* realization =
        &record->access.dynamic_realizations[i];
    const uint16_t mask = loom_low_lower_memory_term_range(
        realization->first_term, realization->term_count);
    const uint32_t hash =
        loom_low_lower_memory_component_hash(&record->access, mask);
    loom_low_lower_memory_component_entry_t* entry =
        loom_low_lower_memory_component_find(builder, &record->access, mask,
                                             hash);
    if (entry == NULL) {
      if (builder->entry_count >= builder->bucket_count * 3 / 4) {
        IREE_RETURN_IF_ERROR(loom_low_lower_memory_components_grow(builder));
      }
      IREE_RETURN_IF_ERROR(
          iree_arena_allocate(builder->arena, sizeof(*entry), (void**)&entry));
      const iree_host_size_t bucket = hash & (builder->bucket_count - 1);
      *entry = (loom_low_lower_memory_component_entry_t){
          .key_access = &record->access,
          .key_mask = mask,
          .hash = hash,
          .next = builder->buckets[bucket],
      };
      builder->buckets[bucket] = entry;
      ++builder->entry_count;
    }
    // An enclosing realization already serves this scope and its descendants.
    // Keeping it avoids extending a redundant local expression's live range.
    if (entry->candidate != NULL) {
      continue;
    }
    loom_low_lower_memory_component_candidate_t* candidate = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate(builder->arena, sizeof(*candidate),
                                             (void**)&candidate));
    *candidate = (loom_low_lower_memory_component_candidate_t){
        .record = record,
        .term = &realization->term,
        .term_mask = mask,
    };
    entry->candidate = candidate;
    entry->scope_next = builder->scope->first;
    builder->scope->first = entry;
  }
  return iree_ok_status();
}

iree_status_t loom_low_lower_source_memory_builder_create(
    loom_low_lower_context_t* context,
    loom_low_lower_source_memory_builder_t** out_builder) {
  loom_low_lower_source_memory_builder_t* builder = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(&context->planning_arena,
                                           sizeof(*builder), (void**)&builder));
  *builder = (loom_low_lower_source_memory_builder_t){
      .arena = &context->planning_arena,
      .cfg = loom_low_lower_context_cfg(context),
  };
  *out_builder = builder;
  return iree_ok_status();
}

iree_status_t loom_low_lower_source_memory_observe(
    loom_low_lower_source_memory_builder_t* builder,
    loom_low_lower_context_t* context, const loom_op_t* source_op) {
  context->lowering.source_plan.memory.current = NULL;
  if (!loom_memory_access_isa(
          loom_memory_access_cast(context->module, source_op)) &&
      !loom_buffer_view_isa(source_op) && !loom_view_subview_isa(source_op)) {
    return iree_ok_status();
  }
  const loom_view_region_table_t* view_regions = NULL;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_context_view_regions(context, &view_regions));
  loom_low_lower_source_memory_record_t* record = NULL;
  IREE_RETURN_IF_ERROR(loom_low_lower_allocate_function_array(
      context, 1, sizeof(*record), (void**)&record));
  *record = (loom_low_lower_source_memory_record_t){
      .source_op = source_op,
      .prepared_plan = loom_low_lower_plan_empty(),
  };
  record->available = loom_low_source_memory_access_plan_build(
      view_regions, source_op, &record->access, &record->diagnostic);
  if (record->available) {
    record->invariant_term_mask =
        loom_low_lower_memory_invariant_terms(context, record);
    loom_low_lower_memory_component_select(builder, record,
                                           record->invariant_term_mask);
    if (record->access.retained_component.term == NULL) {
      loom_low_lower_memory_component_select(
          builder, record,
          loom_low_lower_memory_term_range(0,
                                           record->access.dynamic_term_count));
    }
    IREE_RETURN_IF_ERROR(
        loom_low_lower_memory_components_publish(builder, record));
  }
  if (builder->last != NULL) {
    builder->last->next = record;
  } else {
    context->lowering.source_plan.memory.first = record;
  }
  builder->last = record;
  context->lowering.source_plan.memory.current = record;
  return iree_ok_status();
}

iree_status_t loom_low_lower_source_memory_prepare(
    loom_low_lower_context_t* context) {
  const loom_low_lower_select_op_callback_t callback =
      context->policy->prepare_source_memory;
  if (callback.fn == NULL) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_low_lower_realizations_create(context));
  iree_status_t status = iree_ok_status();
  for (loom_low_lower_source_memory_record_t* record =
           context->lowering.source_plan.memory.first;
       record && iree_status_is_ok(status) &&
       !loom_low_lower_context_should_stop(context);
       record = record->next) {
    if (!record->available || !loom_memory_access_isa(loom_memory_access_cast(
                                  context->module, record->source_op))) {
      continue;
    }
    context->lowering.source_plan.memory.current = record;
    context->planning_arena_active = true;
    status = callback.fn(callback.user_data, context, record->source_op,
                         &record->prepared_plan);
    context->planning_arena_active = false;
    iree_arena_reset(&context->planning_arena);
  }
  context->lowering.source_plan.memory.current = NULL;
  if (iree_status_is_ok(status) &&
      !loom_low_lower_context_should_stop(context)) {
    status = loom_low_lower_realizations_finalize(context);
  }
  return status;
}

void loom_low_lower_source_memory_select_op(loom_low_lower_context_t* context,
                                            const loom_op_t* source_op) {
  loom_low_lower_source_plan_t* plan = &context->lowering.source_plan;
  const loom_low_lower_source_memory_record_t* record = plan->memory.cursor;
  plan->memory.current = NULL;
  if (record != NULL && record->source_op == source_op) {
    plan->memory.current = record;
    plan->memory.cursor = record->next;
  }
}

const loom_low_source_memory_access_plan_t* loom_low_lower_source_memory_access(
    const loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_source_memory_access_diagnostic_t* out_diagnostic) {
  const loom_low_lower_source_memory_record_t* record =
      context->lowering.source_plan.memory.current;
  IREE_ASSERT(record != NULL && record->source_op == source_op);
  *out_diagnostic = record->diagnostic;
  return record->available ? &record->access : NULL;
}

uint16_t loom_low_lower_source_memory_invariant_terms(
    const loom_low_lower_context_t* context) {
  return context->lowering.source_plan.memory.current->invariant_term_mask;
}
