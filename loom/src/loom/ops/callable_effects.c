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
              : loom_func_like_purity(function) == 0;
}

bool loom_callable_effects_may_write(loom_func_like_t function) {
  if (!loom_func_like_isa(function)) {
    return true;
  }
  loom_region_t* body = loom_func_like_body(function);
  return body ? loom_region_has_write_effects(body)
              : loom_func_like_purity(function) == 0;
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
  return loom_func_like_purity(function) != 0;
}

bool loom_callable_effects_callee_is_pure(const loom_module_t* module,
                                          loom_symbol_ref_t callee) {
  if (!loom_symbol_ref_is_valid(callee) || callee.module_id != 0 ||
      callee.symbol_id >= module->symbols.count) {
    return false;
  }
  const loom_symbol_t* symbol = &module->symbols.entries[callee.symbol_id];
  if (!symbol->defining_op) {
    return false;
  }
  return loom_callable_effects_is_pure(
      loom_func_like_cast(module, symbol->defining_op));
}

loom_trait_flags_t loom_callable_effects_traits(uint8_t purity) {
  return LOOM_TRAIT_CALLABLE_BOUNDARY |
         (purity ? LOOM_TRAIT_PURE : LOOM_TRAIT_UNKNOWN_EFFECTS);
}
