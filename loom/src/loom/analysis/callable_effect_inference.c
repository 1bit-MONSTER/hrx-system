// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/callable_effect_inference.h"

#include <string.h>

#include "iree/base/internal/arena.h"
#include "loom/ir/module.h"
#include "loom/ops/callable_effects.h"
#include "loom/ops/op_defs.h"
#include "loom/util/call_graph.h"
#include "loom/util/walk.h"

typedef struct loom_callable_effect_scc_t {
  // Completed SCC contract after bottom-up propagation.
  loom_callable_effect_contract_t contract;
  // First member node in the transient intrusive list, or UINT16_MAX.
  uint16_t first_node;
} loom_callable_effect_scc_t;

typedef struct loom_callable_effect_inference_t {
  // Module being finalized.
  loom_module_t* module;
  // Transient analysis storage.
  iree_arena_allocator_t* arena;
  // Direct call graph and SCC assignment.
  loom_call_graph_t graph;
  // SCC records indexed in callee-first order.
  loom_callable_effect_scc_t* sccs;
  // Next node in the same SCC, indexed by graph node ordinal.
  uint16_t* next_scc_node;
  // Unannotated tracked calls awaiting a completed callee summary.
  loom_op_t** calls;
  // Number of entries in |calls|.
  iree_host_size_t call_count;
  // Allocated entry capacity in |calls|.
  iree_host_size_t call_capacity;
  // SCC receiving effects during the current function walk.
  uint16_t current_scc;
} loom_callable_effect_inference_t;

static loom_callable_effect_contract_t loom_callable_effect_join(
    loom_callable_effect_contract_t lhs, loom_callable_effect_contract_t rhs) {
  if (lhs == LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED ||
      rhs == LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED) {
    return LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED;
  }
  if (lhs == LOOM_CALLABLE_EFFECT_CONTRACT_READONLY ||
      rhs == LOOM_CALLABLE_EFFECT_CONTRACT_READONLY) {
    return LOOM_CALLABLE_EFFECT_CONTRACT_READONLY;
  }
  return LOOM_CALLABLE_EFFECT_CONTRACT_PURE;
}

static loom_callable_effect_contract_t
loom_callable_effect_explicit_function_contract(const loom_module_t* module,
                                                loom_symbol_id_t symbol_id) {
  if (symbol_id >= module->symbols.count) {
    return LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED;
  }
  const loom_symbol_t* symbol = &module->symbols.entries[symbol_id];
  const loom_func_like_t function =
      loom_func_like_cast(module, symbol->defining_op);
  const loom_callable_effect_contract_t contract =
      loom_func_like_effect_contract(function);
  return contract < LOOM_CALLABLE_EFFECT_CONTRACT_COUNT_
             ? contract
             : LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED;
}

static iree_status_t loom_callable_effect_record_call(
    loom_callable_effect_inference_t* inference, loom_op_t* op) {
  if (inference->call_count == inference->call_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        inference->arena, inference->call_count, inference->call_count + 1,
        sizeof(*inference->calls), &inference->call_capacity,
        (void**)&inference->calls));
  }
  inference->calls[inference->call_count++] = op;
  return iree_ok_status();
}

static iree_status_t loom_callable_effect_classify_op(
    void* user_data, loom_op_t* op, const loom_walk_context_t* context,
    loom_walk_result_t* out_result) {
  (void)context;
  *out_result = LOOM_WALK_CONTINUE;
  loom_callable_effect_inference_t* inference = user_data;
  loom_callable_effect_scc_t* scc = &inference->sccs[inference->current_scc];
  const loom_call_like_t call = loom_call_like_cast(inference->module, op);
  if (loom_call_like_isa(call) &&
      loom_call_graph_tracks_call_kind(loom_call_like_kind(call))) {
    const loom_callable_effect_contract_t authored_contract =
        loom_call_like_effect_contract(call);
    if (authored_contract != LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED) {
      scc->contract =
          loom_callable_effect_join(scc->contract, authored_contract);
    } else if (call.vtable->effect_contract_attr_index !=
               LOOM_ATTR_INDEX_NONE) {
      IREE_RETURN_IF_ERROR(loom_callable_effect_record_call(inference, op));
    }
    return iree_ok_status();
  }

  const loom_callable_effect_contract_t op_contract =
      loom_callable_effects_classify_traits(
          loom_op_effective_traits(inference->module, op));
  scc->contract = loom_callable_effect_join(scc->contract, op_contract);
  return iree_ok_status();
}

static loom_callable_effect_contract_t loom_callable_effect_symbol_contract(
    const loom_callable_effect_inference_t* inference,
    loom_symbol_id_t symbol_id) {
  const loom_call_graph_node_t* node =
      loom_call_graph_node(&inference->graph, symbol_id);
  if (node) {
    return inference->sccs[node->scc_id].contract;
  }
  return loom_callable_effect_explicit_function_contract(inference->module,
                                                         symbol_id);
}

static iree_status_t loom_callable_effect_inference_build(
    loom_callable_effect_inference_t* inference) {
  IREE_RETURN_IF_ERROR(loom_call_graph_build(
      inference->module, inference->arena, &inference->graph));
  if (inference->graph.node_count == 0) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      inference->arena, inference->graph.scc_count, sizeof(*inference->sccs),
      (void**)&inference->sccs));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      inference->arena, inference->graph.node_count,
      sizeof(*inference->next_scc_node), (void**)&inference->next_scc_node));
  for (uint16_t i = 0; i < inference->graph.scc_count; ++i) {
    inference->sccs[i] = (loom_callable_effect_scc_t){
        .contract = LOOM_CALLABLE_EFFECT_CONTRACT_PURE,
        .first_node = UINT16_MAX,
    };
  }

  // Index SCC members while classifying each function's complete root-region
  // set exactly once. Authored definition contracts are stable upper bounds:
  // a read-only definition remains read-only even when its current body is
  // accidentally pure.
  for (uint16_t i = 0; i < inference->graph.node_count; ++i) {
    const loom_call_graph_node_t* node = &inference->graph.nodes[i];
    loom_callable_effect_scc_t* scc = &inference->sccs[node->scc_id];
    inference->next_scc_node[i] = scc->first_node;
    scc->first_node = i;
    const loom_callable_effect_contract_t authored_contract =
        loom_callable_effect_explicit_function_contract(inference->module,
                                                        node->symbol_id);
    if (authored_contract != LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED) {
      scc->contract =
          loom_callable_effect_join(scc->contract, authored_contract);
    }

    const loom_symbol_t* symbol =
        &inference->module->symbols.entries[node->symbol_id];
    const loom_func_like_t function =
        loom_func_like_cast(inference->module, symbol->defining_op);
    inference->current_scc = node->scc_id;
    loom_walk_result_t walk_result = LOOM_WALK_CONTINUE;
    IREE_RETURN_IF_ERROR(loom_walk_function(
        inference->module, function, LOOM_WALK_PRE_ORDER,
        (loom_walk_callback_t){loom_callable_effect_classify_op, inference},
        &walk_result));
  }

  // Tarjan assigns cross-SCC callees lower IDs than their callers. Fold each
  // completed callee contract into its caller exactly once per deduplicated
  // call-graph edge.
  for (uint16_t scc_id = 0; scc_id < inference->graph.scc_count; ++scc_id) {
    loom_callable_effect_scc_t* scc = &inference->sccs[scc_id];
    for (uint16_t node_index = scc->first_node; node_index != UINT16_MAX;
         node_index = inference->next_scc_node[node_index]) {
      const loom_call_graph_node_t* node = &inference->graph.nodes[node_index];
      for (uint16_t i = 0; i < node->callee_count; ++i) {
        const loom_symbol_id_t callee_symbol_id = node->callees[i];
        const loom_call_graph_node_t* callee_node =
            loom_call_graph_node(&inference->graph, callee_symbol_id);
        if (callee_node && callee_node->scc_id == scc_id) {
          continue;
        }
        if (callee_node) {
          IREE_ASSERT_LT(callee_node->scc_id, scc_id);
        }
        scc->contract = loom_callable_effect_join(
            scc->contract,
            loom_callable_effect_symbol_contract(inference, callee_symbol_id));
      }
    }
  }

  // Calls retained during classification now receive a durable contract. The
  // generic setter refreshes effective traits and all ancestor summaries, so
  // verification and later passes never need to repeat this graph analysis.
  for (iree_host_size_t i = 0; i < inference->call_count; ++i) {
    const loom_call_like_t call =
        loom_call_like_cast(inference->module, inference->calls[i]);
    const loom_symbol_ref_t callee = loom_call_like_callee(call);
    const loom_callable_effect_contract_t contract =
        loom_callable_effect_symbol_contract(inference, callee.symbol_id);
    if (contract != LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED) {
      loom_call_like_set_effect_contract(inference->module, call, contract);
    }
  }
  return iree_ok_status();
}

iree_status_t loom_callable_effect_inference_run(loom_module_t* module) {
  IREE_ASSERT_ARGUMENT(module);
  iree_arena_allocator_t arena;
  iree_arena_initialize(module->arena.block_pool, &arena);
  loom_callable_effect_inference_t inference = {
      .module = module,
      .arena = &arena,
  };
  const iree_status_t status = loom_callable_effect_inference_build(&inference);
  iree_arena_deinitialize(&arena);
  return status;
}
