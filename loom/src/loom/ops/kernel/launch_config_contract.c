// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/kernel/launch_config_contract.h"

#include <string.h>

#include "loom/analysis/value_relation.h"
#include "loom/ir/module.h"
#include "loom/ops/kernel/launch_config.h"
#include "loom/ops/kernel/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/util/fact_table.h"
#include "loom/util/reference_facts.h"
#include "loom/util/walk.h"

typedef struct loom_kernel_launch_config_contract_analysis_t {
  // Module containing the kernel definition.
  const loom_module_t* module;
  // Kernel definition whose configuration is being analyzed.
  const loom_op_t* kernel_op;
  // Configuration region projected from the kernel workload signature.
  loom_region_t* config_region;
  // Function-like view of kernel_op.
  loom_func_like_t function;
  // Root-region ordinal of config_region in function.
  uint8_t config_region_index;
  // Caller-owned transient storage.
  iree_arena_allocator_t* arena;
  // Facts computed once for config_region and its nested regions.
  loom_value_fact_table_t facts;
  // Read operations retained in source order during effect classification.
  const loom_op_t** read_ops;
  // Number of entries in read_ops.
  iree_host_size_t read_op_count;
  // Allocated entry capacity in read_ops.
  iree_host_size_t read_op_capacity;
  // Module-value-indexed storage-dependence membership bits.
  uint64_t* dependent_bits;
  // Number of entries in dependent_bits.
  iree_host_size_t dependent_word_count;
  // Worklist of newly dependent values.
  loom_value_id_t* dependent_values;
  // Number of initialized entries in dependent_values.
  iree_host_size_t dependent_value_count;
  // Allocated entry capacity in dependent_values.
  iree_host_size_t dependent_value_capacity;
  // Analysis result populated at the first semantic failure.
  loom_kernel_launch_config_contract_t* contract;
} loom_kernel_launch_config_contract_analysis_t;

static void loom_kernel_launch_config_contract_fail(
    loom_kernel_launch_config_contract_analysis_t* analysis,
    loom_kernel_launch_config_contract_failure_t failure, const loom_op_t* op) {
  if (analysis->contract->failure !=
      LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_NONE) {
    return;
  }
  analysis->contract->failure = failure;
  analysis->contract->failure_op = op;
}

static iree_status_t loom_kernel_launch_config_contract_record_read(
    loom_kernel_launch_config_contract_analysis_t* analysis,
    const loom_op_t* op) {
  if (analysis->read_op_count == analysis->read_op_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        analysis->arena, analysis->read_op_count, analysis->read_op_count + 1,
        sizeof(*analysis->read_ops), &analysis->read_op_capacity,
        (void**)&analysis->read_ops));
  }
  analysis->read_ops[analysis->read_op_count++] = op;
  return iree_ok_status();
}

static iree_status_t loom_kernel_launch_config_contract_classify_op(
    void* user_data, loom_op_t* op, const loom_walk_context_t* context,
    loom_walk_result_t* out_result) {
  (void)context;
  *out_result = LOOM_WALK_CONTINUE;
  loom_kernel_launch_config_contract_analysis_t* analysis = user_data;
  const loom_trait_flags_t traits =
      loom_op_effective_traits(analysis->module, op);
  const loom_trait_flags_t forbidden_traits =
      LOOM_TRAIT_WRITES_MEMORY | LOOM_TRAIT_NON_DETERMINISTIC |
      LOOM_TRAIT_UNKNOWN_EFFECTS | LOOM_TRAIT_UNIQUE_IDENTITY |
      LOOM_TRAIT_CONVERGENT | LOOM_TRAIT_MEMORY_FENCE |
      LOOM_TRAIT_OBSERVABLE_EFFECT;
  if (iree_any_bit_set(traits, forbidden_traits)) {
    loom_kernel_launch_config_contract_fail(
        analysis, LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_EFFECT, op);
    analysis->contract->failure_traits = traits;
    *out_result = LOOM_WALK_ABORT;
    return iree_ok_status();
  }
  if (!iree_any_bit_set(traits, LOOM_TRAIT_READS_MEMORY)) {
    return iree_ok_status();
  }

  const loom_memory_access_t access =
      loom_memory_access_cast(analysis->module, op);
  if (!loom_memory_access_isa(access) ||
      loom_memory_access_operation_kind(access) !=
          LOOM_MEMORY_ACCESS_OPERATION_LOAD) {
    loom_kernel_launch_config_contract_fail(
        analysis, LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_READ_OPERATION,
        op);
    *out_result = LOOM_WALK_ABORT;
    return iree_ok_status();
  }
  analysis->contract->effect_contract = LOOM_CALLABLE_EFFECT_CONTRACT_READONLY;
  return loom_kernel_launch_config_contract_record_read(analysis, op);
}

static bool loom_kernel_launch_config_contract_read_origin_is_workload(
    const loom_kernel_launch_config_contract_analysis_t* analysis,
    const loom_op_t* read_op) {
  const loom_memory_access_t access =
      loom_memory_access_cast(analysis->module, read_op);
  const loom_value_id_t storage = loom_memory_access_view(access);
  if (storage == LOOM_VALUE_ID_INVALID ||
      storage >= analysis->module->values.count) {
    return false;
  }
  const loom_value_facts_t facts =
      loom_value_fact_table_lookup(&analysis->facts, storage);
  loom_value_fact_reference_origin_t origin = {0};
  loom_value_fact_buffer_reference_t buffer_reference = {0};
  loom_value_fact_view_reference_t view_reference = {0};
  if (loom_value_facts_query_buffer_reference(&analysis->facts.context, facts,
                                              &buffer_reference)) {
    origin = buffer_reference.origin;
  } else if (loom_value_facts_query_view_reference(&analysis->facts.context,
                                                   facts, &view_reference)) {
    origin = view_reference.origin;
  } else {
    return false;
  }
  return origin.kind == LOOM_VALUE_FACT_REFERENCE_ORIGIN_ENTRY &&
         origin.function_symbol_id ==
             loom_func_like_callee(analysis->function).symbol_id &&
         origin.region_index == analysis->config_region_index;
}

static bool loom_kernel_launch_config_contract_value_is_dependent(
    const loom_kernel_launch_config_contract_analysis_t* analysis,
    loom_value_id_t value_id) {
  return value_id < analysis->module->values.count &&
         (analysis->dependent_bits[value_id / 64] &
          (UINT64_C(1) << (value_id % 64))) != 0;
}

static iree_status_t loom_kernel_launch_config_contract_mark_dependent(
    loom_kernel_launch_config_contract_analysis_t* analysis,
    loom_value_id_t value_id) {
  if (value_id == LOOM_VALUE_ID_INVALID ||
      value_id >= analysis->module->values.count ||
      loom_kernel_launch_config_contract_value_is_dependent(analysis,
                                                            value_id)) {
    return iree_ok_status();
  }
  analysis->dependent_bits[value_id / 64] |= UINT64_C(1) << (value_id % 64);
  if (analysis->dependent_value_count == analysis->dependent_value_capacity) {
    IREE_RETURN_IF_ERROR(
        iree_arena_grow_array(analysis->arena, analysis->dependent_value_count,
                              analysis->dependent_value_count + 1,
                              sizeof(*analysis->dependent_values),
                              &analysis->dependent_value_capacity,
                              (void**)&analysis->dependent_values));
  }
  analysis->dependent_values[analysis->dependent_value_count++] = value_id;
  return iree_ok_status();
}

static iree_status_t loom_kernel_launch_config_contract_mark_results(
    loom_kernel_launch_config_contract_analysis_t* analysis,
    const loom_op_t* op) {
  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_kernel_launch_config_contract_mark_dependent(
        analysis, results[i]));
  }
  return iree_ok_status();
}

static iree_status_t loom_kernel_launch_config_contract_propagate_user(
    loom_kernel_launch_config_contract_analysis_t* analysis,
    loom_value_id_t source, const loom_op_t* user, uint16_t operand_index) {
  // Preserve precise data transport across identity/select operations and
  // structured control flow. Tied-result and reference-source relations only
  // describe storage identity, not exhaustive data dependence, so those uses
  // retain the conservative all-results behavior below.
  const loom_value_relation_mask_t relation_mask =
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_FACT_IDENTITY) |
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_VALUE_ALIAS) |
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_SELECT_PAYLOAD) |
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_ELEMENTWISE) |
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_CFG_ARGUMENT) |
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_LOOP_CARRIED) |
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_LOOP_BYPASS) |
      LOOM_VALUE_RELATION_MASK(LOOM_VALUE_RELATION_REGION_RESULT);
  bool has_operand_relation = false;
  loom_value_relation_iterator_t relations;
  loom_value_relation_iterator_initialize(analysis->module, user, relation_mask,
                                          &relations);
  loom_value_relation_t relation;
  while (loom_value_relation_iterator_next(&relations, &relation)) {
    if (relation.source_value_id != source ||
        relation.source_operand_index != operand_index) {
      continue;
    }
    has_operand_relation = true;
    IREE_RETURN_IF_ERROR(loom_kernel_launch_config_contract_mark_dependent(
        analysis, relation.destination_value_id));
  }

  // An operand use without an exhaustive relation may contribute to every
  // direct result. A resultless control terminator instead controls the values
  // produced by its region owner, such as scf.condition controlling scf.while
  // results or cfg.cond_br controlling an enclosing region branch.
  if (!has_operand_relation) {
    IREE_RETURN_IF_ERROR(
        loom_kernel_launch_config_contract_mark_results(analysis, user));
    if (user->result_count == 0 && user->parent_op != NULL &&
        loom_op_operand_has_role(analysis->module, user, operand_index,
                                 LOOM_OPERAND_ROLE_CONTROL_CONDITION)) {
      IREE_RETURN_IF_ERROR(loom_kernel_launch_config_contract_mark_results(
          analysis, user->parent_op));
    }
  }
  return iree_ok_status();
}

static bool loom_kernel_launch_config_contract_operand_is_size(
    const loom_op_t* launch_config, uint16_t operand_index,
    loom_kernel_launch_config_contract_failure_t* out_failure,
    uint8_t* out_dimension) {
  if (operand_index >= 3 && operand_index < 6) {
    *out_failure = LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_WORKGROUP_SIZE;
    *out_dimension = (uint8_t)(operand_index - 3);
    return true;
  }
  if (operand_index >= 6 && operand_index < launch_config->operand_count) {
    *out_failure = LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_CLUSTER_SIZE;
    *out_dimension = (uint8_t)(operand_index - 6);
    return true;
  }
  return false;
}

static iree_status_t loom_kernel_launch_config_contract_propagate(
    loom_kernel_launch_config_contract_analysis_t* analysis) {
  for (iree_host_size_t cursor = 0;
       cursor < analysis->dependent_value_count &&
       analysis->contract->failure ==
           LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_NONE;
       ++cursor) {
    const loom_value_id_t value_id = analysis->dependent_values[cursor];
    const loom_value_t* value = loom_module_value(analysis->module, value_id);
    const loom_use_t* uses = loom_value_uses(value);
    for (uint32_t i = 0; i < value->use_count; ++i) {
      const loom_op_t* user = loom_use_user_op(uses[i]);
      const uint16_t operand_index = loom_use_operand_index(uses[i]);
      if (loom_kernel_launch_config_isa(user)) {
        loom_kernel_launch_config_contract_failure_t failure =
            LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_NONE;
        uint8_t dimension = 0;
        if (loom_kernel_launch_config_contract_operand_is_size(
                user, operand_index, &failure, &dimension)) {
          loom_kernel_launch_config_contract_fail(analysis, failure, user);
          analysis->contract->failure_dimension = dimension;
          break;
        }
        continue;
      }
      IREE_RETURN_IF_ERROR(loom_kernel_launch_config_contract_propagate_user(
          analysis, value_id, user, operand_index));
    }

    loom_type_use_iterator_t type_users;
    loom_module_value_type_users(analysis->module, value_id, &type_users);
    for (loom_value_id_t carrier = loom_type_users_next(&type_users);
         carrier != LOOM_VALUE_ID_INVALID;
         carrier = loom_type_users_next(&type_users)) {
      IREE_RETURN_IF_ERROR(
          loom_kernel_launch_config_contract_mark_dependent(analysis, carrier));
    }

    loom_type_use_iterator_t attribute_users;
    loom_attribute_users_begin(&analysis->module->type_uses, value_id,
                               &attribute_users);
    for (loom_attribute_user_t user =
             loom_attribute_users_next(&attribute_users);
         user.op != NULL; user = loom_attribute_users_next(&attribute_users)) {
      const loom_value_id_t* results = loom_op_const_results(user.op);
      for (uint16_t i = 0; i < user.op->result_count; ++i) {
        IREE_RETURN_IF_ERROR(loom_kernel_launch_config_contract_mark_dependent(
            analysis, results[i]));
      }
    }
  }
  return iree_ok_status();
}

static void loom_kernel_launch_config_contract_check_count_ranges(
    loom_kernel_launch_config_contract_analysis_t* analysis) {
  const loom_op_t* launch_config =
      loom_kernel_def_launch_config_op(analysis->kernel_op);
  for (uint8_t dimension = 0; dimension < 3; ++dimension) {
    const loom_value_id_t count =
        loom_kernel_launch_config_workgroup_count_operand(
            launch_config, (loom_kernel_dimension_t)dimension);
    if (!loom_kernel_launch_config_contract_value_is_dependent(analysis,
                                                               count)) {
      continue;
    }
    const loom_value_facts_t facts =
        loom_value_fact_table_lookup(&analysis->facts, count);
    if (!loom_value_facts_is_float(facts) && facts.range_lo >= 0 &&
        facts.range_hi <= UINT32_MAX) {
      continue;
    }
    loom_kernel_launch_config_contract_fail(
        analysis, LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_WORKGROUP_COUNT,
        launch_config);
    analysis->contract->failure_dimension = dimension;
    analysis->contract->failure_range_lower = facts.range_lo;
    analysis->contract->failure_range_upper = facts.range_hi;
    return;
  }
}

iree_status_t loom_kernel_launch_config_contract_analyze(
    const loom_module_t* module, const loom_op_t* kernel_op,
    iree_arena_allocator_t* scratch_arena,
    loom_kernel_launch_config_contract_t* out_contract) {
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(kernel_op);
  IREE_ASSERT_ARGUMENT(scratch_arena);
  IREE_ASSERT_ARGUMENT(out_contract);
  IREE_ASSERT(loom_kernel_def_isa(kernel_op));
  *out_contract = (loom_kernel_launch_config_contract_t){
      .effect_contract = LOOM_CALLABLE_EFFECT_CONTRACT_PURE,
  };

  loom_kernel_launch_config_contract_analysis_t analysis = {
      .module = module,
      .kernel_op = kernel_op,
      .config_region = loom_kernel_def_config(kernel_op),
      .function = loom_func_like_const_cast(module, kernel_op),
      .config_region_index = LOOM_REGION_INDEX_NONE,
      .arena = scratch_arena,
      .contract = out_contract,
  };
  for (uint8_t i = 0; i < loom_func_like_region_count(analysis.function); ++i) {
    if (loom_func_like_region(analysis.function, i) == analysis.config_region) {
      analysis.config_region_index = i;
      break;
    }
  }
  IREE_ASSERT_NE(analysis.config_region_index, LOOM_REGION_INDEX_NONE);

  analysis.dependent_word_count = (module->values.count + 63) / 64;
  if (analysis.dependent_word_count != 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        scratch_arena, analysis.dependent_word_count,
        sizeof(*analysis.dependent_bits), (void**)&analysis.dependent_bits));
    memset(analysis.dependent_bits, 0,
           analysis.dependent_word_count * sizeof(*analysis.dependent_bits));
  }

  loom_walk_result_t walk_result = LOOM_WALK_CONTINUE;
  IREE_RETURN_IF_ERROR(
      loom_walk_region(module, analysis.config_region, LOOM_WALK_PRE_ORDER,
                       (loom_walk_callback_t){
                           .fn = loom_kernel_launch_config_contract_classify_op,
                           .user_data = &analysis,
                       },
                       &walk_result));
  if (out_contract->failure !=
      LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_NONE) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_value_fact_table_initialize(
      &analysis.facts, scratch_arena, /*initial_capacity=*/0));
  IREE_RETURN_IF_ERROR(loom_value_fact_table_compute_region(
      &analysis.facts, module, analysis.function, analysis.config_region,
      (loom_op_t*)kernel_op));
  for (iree_host_size_t i = 0; i < analysis.read_op_count; ++i) {
    const loom_op_t* read_op = analysis.read_ops[i];
    if (!loom_kernel_launch_config_contract_read_origin_is_workload(&analysis,
                                                                    read_op)) {
      loom_kernel_launch_config_contract_fail(
          &analysis, LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_READ_ORIGIN,
          read_op);
      return iree_ok_status();
    }
    const loom_value_id_t* results = loom_op_const_results(read_op);
    for (uint16_t j = 0; j < read_op->result_count; ++j) {
      IREE_RETURN_IF_ERROR(loom_kernel_launch_config_contract_mark_dependent(
          &analysis, results[j]));
    }
  }

  IREE_RETURN_IF_ERROR(loom_kernel_launch_config_contract_propagate(&analysis));
  if (out_contract->failure ==
      LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_NONE) {
    loom_kernel_launch_config_contract_check_count_ranges(&analysis);
  }
  return iree_ok_status();
}
