// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/low/capture.h"

#include "loom/ir/module.h"

static bool loom_low_capture_source_has_required_identity(
    const loom_value_t* source_value) {
  if (!loom_value_is_block_arg(source_value)) {
    const loom_op_t* definition = loom_value_def_op(source_value);
    if (definition && loom_traits_have_storage_relation(definition->traits) &&
        (loom_traits_are_fact_identity(definition->traits) ||
         loom_traits_are_value_alias(definition->traits))) {
      // A nonwriting required alias shares ownership with its source, whose
      // consumers are absent from this value's use list. A destructive result
      // instead starts new ownership: the prior value has been consumed.
      return true;
    }
  }
  return false;
}

bool loom_low_capture_source_is_stable(const loom_module_t* module,
                                       loom_value_id_t source) {
  const loom_value_t* source_value = loom_module_value(module, source);
  return !loom_low_capture_source_has_required_identity(source_value) &&
         loom_value_ownership_use_count(source_value) == 0;
}

bool loom_low_capture_can_forward(const loom_module_t* module,
                                  loom_value_id_t source,
                                  loom_value_id_t result,
                                  uint16_t source_occurrences) {
  const loom_value_t* source_value = loom_module_value(module, source);
  if (loom_low_capture_source_has_required_identity(source_value)) {
    return false;
  }
  if (loom_value_ownership_use_count(source_value) == 0 &&
      loom_value_ownership_use_count(loom_module_value(module, result)) == 0) {
    return true;
  }
  // A sole observation can transfer the independent source's ownership to all
  // result uses without merging two separately observed owners.
  return source_value->use_count == source_occurrences &&
         !loom_value_has_attribute_uses(source_value) &&
         !loom_module_value_has_type_uses(module, source);
}
