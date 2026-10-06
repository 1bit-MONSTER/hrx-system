// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/source_value_analysis.h"

#include <stdint.h>
#include <string.h>

#include "loom/analysis/view_regions.h"
#include "loom/codegen/low/source_memory_plan.h"
#include "loom/ir/context.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/cfg/ops.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/kernel/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/scf/ops.h"
#include "loom/ops/vector/ops.h"
#include "loom/ops/view/ops.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/arch/amdgpu/target_info_defs.h"
#include "loom/util/cfg_graph.h"
#include "loom/util/fact_table.h"

static int loom_amdgpu_source_value_analysis_state_key;
static iree_status_t loom_amdgpu_source_value_analysis_prepare(
    const loom_value_fact_table_t* fact_table,
    const loom_local_value_domain_t* value_domain,
    const loom_low_descriptor_set_t* descriptor_set,
    iree_arena_allocator_t* arena,
    loom_amdgpu_source_value_analysis_t* analysis) {
  if (analysis->descriptor_set != descriptor_set) {
    analysis->descriptor_set = descriptor_set;
    const loom_amdgpu_descriptor_set_info_t* descriptor_set_info =
        descriptor_set != NULL ? loom_amdgpu_target_info_descriptor_set_at(
                                     descriptor_set->descriptor_set_ordinal)
                               : NULL;
    analysis->descriptor_set_info_flags =
        descriptor_set_info != NULL ? descriptor_set_info->flags : 0;
    analysis->value_domain = NULL;
    analysis->fact_table = NULL;
  }
  if (analysis->value_domain != value_domain ||
      analysis->fact_table != fact_table ||
      (value_domain != NULL &&
       analysis->record_count < value_domain->value_count)) {
    analysis->value_domain = value_domain;
    analysis->fact_table = fact_table;
    analysis->records = NULL;
    analysis->record_count =
        value_domain != NULL ? value_domain->value_count : 0;
    analysis->query.provisional_ordinals = NULL;
    analysis->query.provisional_count = 0;
    analysis->query.generation = 0;
    analysis->query.depth = 0;
    if (analysis->record_count != 0) {
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          arena, analysis->record_count, sizeof(*analysis->records),
          (void**)&analysis->records));
      memset(analysis->records, 0,
             analysis->record_count * sizeof(*analysis->records));
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          arena, analysis->record_count,
          sizeof(*analysis->query.provisional_ordinals),
          (void**)&analysis->query.provisional_ordinals));
    }
  }
  return iree_ok_status();
}

iree_status_t loom_amdgpu_source_value_analysis_for_context(
    loom_low_lower_context_t* context,
    loom_amdgpu_source_value_analysis_t** out_analysis) {
  *out_analysis = NULL;
  loom_amdgpu_source_value_analysis_t* analysis = NULL;
  IREE_RETURN_IF_ERROR(loom_low_lower_get_or_allocate_target_state(
      context, &loom_amdgpu_source_value_analysis_state_key, sizeof(*analysis),
      (void**)&analysis));
  IREE_RETURN_IF_ERROR(loom_amdgpu_source_value_analysis_prepare(
      loom_low_lower_context_fact_table(context),
      loom_low_lower_context_value_domain(context),
      loom_low_lower_context_descriptor_set(context),
      loom_low_lower_context_function_arena(context), analysis));
  *out_analysis = analysis;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_source_value_analysis_for_target_low_legality(
    loom_target_low_legality_context_t* context,
    loom_amdgpu_source_value_analysis_t** out_analysis) {
  *out_analysis = NULL;
  loom_amdgpu_source_value_analysis_t* analysis = NULL;
  IREE_RETURN_IF_ERROR(loom_target_low_legality_get_or_allocate_target_state(
      context, &loom_amdgpu_source_value_analysis_state_key, sizeof(*analysis),
      (void**)&analysis));
  IREE_RETURN_IF_ERROR(loom_amdgpu_source_value_analysis_prepare(
      loom_target_low_legality_fact_table(context),
      loom_target_low_legality_value_domain(context),
      loom_target_low_legality_descriptor_set(context),
      loom_target_low_legality_scratch_arena(context), analysis));
  *out_analysis = analysis;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_source_value_analysis_for_contract_query(
    const loom_target_contract_query_environment_t* environment,
    loom_amdgpu_source_value_analysis_t** out_analysis) {
  *out_analysis = NULL;
  if (environment->value_domain == NULL ||
      environment->target_state_allocator.fn == NULL) {
    return iree_ok_status();
  }
  loom_amdgpu_source_value_analysis_t* analysis = NULL;
  IREE_RETURN_IF_ERROR(loom_target_contract_query_get_or_allocate_target_state(
      environment, &loom_amdgpu_source_value_analysis_state_key,
      sizeof(*analysis), (void**)&analysis));
  if (analysis == NULL) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_amdgpu_source_value_analysis_prepare(
      environment->fact_table, environment->value_domain,
      environment->descriptor_set, environment->arena, analysis));
  *out_analysis = analysis;
  return iree_ok_status();
}

const loom_cfg_graph_t* loom_amdgpu_source_value_analysis_cfg_graph(
    const loom_amdgpu_source_value_analysis_t* analysis,
    const loom_region_t* region) {
  return analysis && analysis->fact_table
             ? loom_value_fact_table_lookup_cfg_graph(analysis->fact_table,
                                                      region)
             : NULL;
}

static loom_amdgpu_source_value_analysis_record_t*
loom_amdgpu_source_value_analysis_lookup(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_value_id_t source_value_id) {
  if (analysis == NULL || analysis->value_domain == NULL ||
      !loom_local_value_domain_is_acquired(analysis->value_domain)) {
    return NULL;
  }
  const loom_value_ordinal_t value_ordinal =
      loom_local_value_domain_try_ordinal(analysis->value_domain,
                                          source_value_id);
  if (value_ordinal == LOOM_VALUE_ORDINAL_INVALID ||
      value_ordinal >= analysis->record_count) {
    return NULL;
  }
  return &analysis->records[value_ordinal];
}

bool loom_amdgpu_source_value_analysis_cached_bit(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_value_id_t source_value_id,
    loom_amdgpu_source_value_analysis_bits_t bit, bool* out_value) {
  *out_value = false;
  loom_amdgpu_source_value_analysis_record_t* record =
      loom_amdgpu_source_value_analysis_lookup(analysis, source_value_id);
  if (record == NULL || !iree_any_bit_set(record->known_bits, bit)) {
    return false;
  }
  *out_value = iree_any_bit_set(record->value_bits, bit);
  if (iree_any_bit_set(record->provisional_bits, bit)) {
    // The current query now depends on an answer that crossed a recursion cut.
    ++analysis->query.generation;
  }
  return true;
}

bool loom_amdgpu_source_value_analysis_begin_bit(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_value_id_t source_value_id,
    loom_amdgpu_source_value_analysis_bits_t bit,
    loom_amdgpu_source_value_analysis_query_token_t* out_token) {
  *out_token = analysis != NULL ? analysis->query.generation : 0;
  loom_amdgpu_source_value_analysis_record_t* record =
      loom_amdgpu_source_value_analysis_lookup(analysis, source_value_id);
  if (record == NULL) {
    return true;
  }
  if (iree_any_bit_set(record->active_bits, bit)) {
    // False is the conservative seed for the least fixed-point queries. Mark
    // every computation spanning this cut as provisional.
    ++analysis->query.generation;
    return false;
  }
  record->active_bits |= bit;
  ++analysis->query.depth;
  return true;
}

static void loom_amdgpu_source_value_analysis_mark_provisional(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_amdgpu_source_value_analysis_record_t* record,
    loom_value_ordinal_t value_ordinal,
    loom_amdgpu_source_value_analysis_bits_t bit) {
  if (iree_any_bit_set(record->provisional_bits, bit)) {
    return;
  }
  if (record->provisional_bits == 0) {
    analysis->query.provisional_ordinals[analysis->query.provisional_count++] =
        value_ordinal;
  }
  record->provisional_bits |= bit;
}

static void loom_amdgpu_source_value_analysis_complete_query(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_value_ordinal_t root_ordinal,
    loom_amdgpu_source_value_analysis_bits_t root_bit) {
  // The completed root has explored every non-cyclic alternative and reached
  // the least fixed point. Nested answers that crossed a cut were only valid
  // within this traversal and must not make later queries order-dependent.
  for (iree_host_size_t i = 0; i < analysis->query.provisional_count; ++i) {
    const loom_value_ordinal_t value_ordinal =
        analysis->query.provisional_ordinals[i];
    loom_amdgpu_source_value_analysis_record_t* record =
        &analysis->records[value_ordinal];
    loom_amdgpu_source_value_analysis_bits_t discarded_bits =
        record->provisional_bits;
    if (value_ordinal == root_ordinal) {
      discarded_bits &= (loom_amdgpu_source_value_analysis_bits_t)~root_bit;
    }
    record->known_bits &=
        (loom_amdgpu_source_value_analysis_bits_t)~discarded_bits;
    record->value_bits &=
        (loom_amdgpu_source_value_analysis_bits_t)~discarded_bits;
    record->provisional_bits = 0;
  }
  analysis->query.provisional_count = 0;
}

void loom_amdgpu_source_value_analysis_end_bit(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_value_id_t source_value_id,
    loom_amdgpu_source_value_analysis_bits_t bit,
    loom_amdgpu_source_value_analysis_query_token_t token, bool value) {
  loom_amdgpu_source_value_analysis_record_t* record =
      loom_amdgpu_source_value_analysis_lookup(analysis, source_value_id);
  if (record == NULL) {
    return;
  }
  record->active_bits &= (loom_amdgpu_source_value_analysis_bits_t)~bit;
  record->known_bits |= bit;
  if (value) {
    record->value_bits |= bit;
  } else {
    record->value_bits &= (loom_amdgpu_source_value_analysis_bits_t)~bit;
  }
  const loom_value_ordinal_t value_ordinal =
      (loom_value_ordinal_t)(record - analysis->records);
  if (token != analysis->query.generation) {
    loom_amdgpu_source_value_analysis_mark_provisional(analysis, record,
                                                       value_ordinal, bit);
  }
  --analysis->query.depth;
  if (analysis->query.depth == 0 && analysis->query.provisional_count != 0) {
    loom_amdgpu_source_value_analysis_complete_query(analysis, value_ordinal,
                                                     bit);
  }
}

bool loom_amdgpu_source_value_analysis_cached_register_shape(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_value_id_t source_value_id, bool* out_has_shape,
    loom_amdgpu_register_shape_t* out_shape) {
  *out_has_shape = false;
  *out_shape = (loom_amdgpu_register_shape_t){0};
  loom_amdgpu_source_value_analysis_record_t* record =
      loom_amdgpu_source_value_analysis_lookup(analysis, source_value_id);
  if (record == NULL ||
      !iree_any_bit_set(record->known_bits,
                        LOOM_AMDGPU_SOURCE_VALUE_ANALYSIS_REGISTER_SHAPE)) {
    return false;
  }
  *out_shape = record->register_shape;
  *out_has_shape = iree_any_bit_set(
      record->value_bits, LOOM_AMDGPU_SOURCE_VALUE_ANALYSIS_REGISTER_SHAPE);
  return true;
}

void loom_amdgpu_source_value_analysis_record_register_shape(
    loom_amdgpu_source_value_analysis_t* analysis,
    loom_value_id_t source_value_id, const loom_amdgpu_register_shape_t* shape,
    bool valid) {
  loom_amdgpu_source_value_analysis_record_t* record =
      loom_amdgpu_source_value_analysis_lookup(analysis, source_value_id);
  if (record == NULL) {
    return;
  }
  record->known_bits |= LOOM_AMDGPU_SOURCE_VALUE_ANALYSIS_REGISTER_SHAPE;
  if (valid) {
    record->value_bits |= LOOM_AMDGPU_SOURCE_VALUE_ANALYSIS_REGISTER_SHAPE;
    record->register_shape = *shape;
  } else {
    record->value_bits &=
        (loom_amdgpu_source_value_analysis_bits_t)~LOOM_AMDGPU_SOURCE_VALUE_ANALYSIS_REGISTER_SHAPE;
    record->register_shape = (loom_amdgpu_register_shape_t){0};
  }
}

iree_status_t loom_amdgpu_context_value_prefers_vgpr(
    loom_low_lower_context_t* context, loom_value_id_t source_value_id,
    bool* out_prefers_vgpr) {
  const loom_view_region_table_t* view_regions = NULL;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_context_view_regions(context, &view_regions));
  loom_amdgpu_source_value_analysis_t* analysis = NULL;
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_source_value_analysis_for_context(context, &analysis));
  *out_prefers_vgpr = loom_amdgpu_analyzed_source_value_prefers_vgpr(
      loom_low_lower_context_module(context),
      loom_low_lower_context_fact_table(context), view_regions, analysis,
      source_value_id);
  return iree_ok_status();
}

iree_status_t loom_amdgpu_target_low_legality_value_prefers_vgpr(
    loom_target_low_legality_context_t* context,
    loom_value_id_t source_value_id, bool* out_prefers_vgpr) {
  const loom_view_region_table_t* view_regions =
      loom_target_low_legality_view_regions(context);
  loom_amdgpu_source_value_analysis_t* analysis = NULL;
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_source_value_analysis_for_target_low_legality(context,
                                                                &analysis));
  *out_prefers_vgpr = loom_amdgpu_analyzed_source_value_prefers_vgpr(
      loom_target_low_legality_module(context),
      loom_target_low_legality_fact_table(context), view_regions, analysis,
      source_value_id);
  return iree_ok_status();
}

iree_status_t loom_amdgpu_context_value_is_native_i1_mask(
    loom_low_lower_context_t* context, loom_value_id_t source_value_id,
    bool* out_is_native_mask) {
  const loom_view_region_table_t* view_regions = NULL;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_context_view_regions(context, &view_regions));
  loom_amdgpu_source_value_analysis_t* analysis = NULL;
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_source_value_analysis_for_context(context, &analysis));
  *out_is_native_mask = loom_amdgpu_analyzed_source_value_is_native_i1_mask(
      loom_low_lower_context_module(context),
      loom_low_lower_context_fact_table(context), view_regions, analysis,
      source_value_id);
  return iree_ok_status();
}

iree_status_t loom_amdgpu_target_low_legality_value_is_native_i1_mask(
    loom_target_low_legality_context_t* context,
    loom_value_id_t source_value_id, bool* out_is_native_mask) {
  const loom_view_region_table_t* view_regions =
      loom_target_low_legality_view_regions(context);
  loom_amdgpu_source_value_analysis_t* analysis = NULL;
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_source_value_analysis_for_target_low_legality(context,
                                                                &analysis));
  *out_is_native_mask = loom_amdgpu_analyzed_source_value_is_native_i1_mask(
      loom_target_low_legality_module(context),
      loom_target_low_legality_fact_table(context), view_regions, analysis,
      source_value_id);
  return iree_ok_status();
}
