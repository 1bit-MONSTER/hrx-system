// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/callable_effects.h"

#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"

bool loom_callable_effects_may_access_memory(loom_func_like_t function) {
  if (!loom_func_like_isa(function)) {
    return true;
  }
  loom_region_t* body = loom_func_like_body(function);
  return body ? loom_region_has_memory_accesses(body)
              : loom_func_like_effect_contract(function) !=
                    LOOM_CALLABLE_EFFECT_CONTRACT_PURE;
}

bool loom_callable_effects_may_write(loom_func_like_t function) {
  if (!loom_func_like_isa(function)) {
    return true;
  }
  loom_region_t* body = loom_func_like_body(function);
  if (body) {
    return loom_region_has_write_effects(body);
  }
  switch (loom_func_like_effect_contract(function)) {
    case LOOM_CALLABLE_EFFECT_CONTRACT_PURE:
    case LOOM_CALLABLE_EFFECT_CONTRACT_READONLY:
      return false;
    default:
      return true;
  }
}

bool loom_callable_effects_is_pure(loom_func_like_t function) {
  if (!loom_func_like_isa(function)) {
    return false;
  }
  loom_region_t* body = loom_func_like_body(function);
  if (body) {
    return !loom_region_has_read_effects(body) &&
           !loom_region_has_write_effects(body) &&
           !loom_region_has_convergent_effects(body) &&
           !loom_region_has_observable_effects(body);
  }
  return loom_func_like_effect_contract(function) ==
         LOOM_CALLABLE_EFFECT_CONTRACT_PURE;
}

bool loom_callable_effects_has_readonly_contract(loom_func_like_t function) {
  return loom_func_like_isa(function) &&
         loom_func_like_effect_contract(function) ==
             LOOM_CALLABLE_EFFECT_CONTRACT_READONLY;
}

loom_callable_effect_contract_t loom_callable_effects_callee_contract(
    const loom_module_t* module, loom_symbol_ref_t callee) {
  if (!loom_symbol_ref_is_valid(callee) || callee.module_id != 0 ||
      callee.symbol_id >= module->symbols.count) {
    return LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED;
  }
  const loom_symbol_t* symbol = &module->symbols.entries[callee.symbol_id];
  if (!symbol->defining_op) {
    return LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED;
  }
  const loom_func_like_t function =
      loom_func_like_cast(module, symbol->defining_op);
  if (loom_callable_effects_is_pure(function)) {
    return LOOM_CALLABLE_EFFECT_CONTRACT_PURE;
  }
  if (loom_callable_effects_has_readonly_contract(function)) {
    return LOOM_CALLABLE_EFFECT_CONTRACT_READONLY;
  }
  return LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED;
}

bool loom_callable_effects_contract_satisfies(
    loom_callable_effect_contract_t provided,
    loom_callable_effect_contract_t required) {
  if (required == LOOM_CALLABLE_EFFECT_CONTRACT_UNSPECIFIED) {
    return true;
  }
  switch (required) {
    case LOOM_CALLABLE_EFFECT_CONTRACT_PURE:
      return provided == LOOM_CALLABLE_EFFECT_CONTRACT_PURE;
    case LOOM_CALLABLE_EFFECT_CONTRACT_READONLY:
      return provided == LOOM_CALLABLE_EFFECT_CONTRACT_PURE ||
             provided == LOOM_CALLABLE_EFFECT_CONTRACT_READONLY;
    default:
      return false;
  }
}

static bool loom_callable_effects_region_is_readonly(
    const loom_module_t* module, const loom_region_t* region) {
  if (!region) {
    return true;
  }
  const loom_trait_flags_t forbidden_traits =
      LOOM_TRAIT_WRITES_MEMORY | LOOM_TRAIT_NON_DETERMINISTIC |
      LOOM_TRAIT_UNKNOWN_EFFECTS | LOOM_TRAIT_UNIQUE_IDENTITY |
      LOOM_TRAIT_CONVERGENT | LOOM_TRAIT_MEMORY_FENCE |
      LOOM_TRAIT_OBSERVABLE_EFFECT;
  const loom_block_t* block = NULL;
  loom_region_for_each_block(region, block) {
    const loom_op_t* op = NULL;
    loom_block_for_each_op(block, op) {
      if (iree_any_bit_set(loom_op_effective_traits(module, op),
                           forbidden_traits)) {
        return false;
      }
      loom_region_t* const* regions = loom_op_regions(op);
      for (uint8_t i = 0; i < op->region_count; ++i) {
        if (!loom_callable_effects_region_is_readonly(module, regions[i])) {
          return false;
        }
      }
    }
  }
  return true;
}

bool loom_callable_effects_body_is_readonly(const loom_module_t* module,
                                            const loom_region_t* body) {
  return loom_callable_effects_region_is_readonly(module, body);
}

loom_trait_flags_t loom_callable_effects_traits(
    loom_callable_effect_contract_t contract) {
  switch (contract) {
    case LOOM_CALLABLE_EFFECT_CONTRACT_PURE:
      return LOOM_TRAIT_CALLABLE_BOUNDARY | LOOM_TRAIT_PURE;
    case LOOM_CALLABLE_EFFECT_CONTRACT_READONLY:
      return LOOM_TRAIT_CALLABLE_BOUNDARY | LOOM_TRAIT_READS_MEMORY;
    default:
      return LOOM_TRAIT_CALLABLE_BOUNDARY | LOOM_TRAIT_UNKNOWN_EFFECTS;
  }
}
