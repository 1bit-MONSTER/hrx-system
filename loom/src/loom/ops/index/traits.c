// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"

// Offset addition and subtraction wrap modulo the selected unsigned carrier.
// The same op kinds use mathematical semantics for index results, whose
// representability depends on retained facts and the selected target carrier.
loom_trait_flags_t loom_index_address_arithmetic_effective_traits(
    const loom_module_t* module, const loom_op_t* op) {
  loom_trait_flags_t traits = op->traits & ~LOOM_TRAIT_SAFE_TO_SPECULATE;
  const loom_type_t result_type =
      loom_module_value_type(module, loom_op_const_results(op)[0]);
  if (loom_type_element_type(result_type) == LOOM_SCALAR_TYPE_OFFSET) {
    traits |= LOOM_TRAIT_SAFE_TO_SPECULATE;
  }
  return traits;
}
