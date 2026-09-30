// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/placement.h"

#include "loom/target/arch/amdgpu/facts.h"

// Relative measured rank is max multiplicity of {A, B, C, C+1} modulo four,
// minus one. Factoring it into flat clauses also gives its exact partial
// minimum when locations are unknown or operands have mandatory identity.
// This is a placement cost, not an instruction legality or hardware latency.
static const loom_low_placement_value_ref_t kMatrixValues[] = {
    {0, LOOM_LOW_PLACEMENT_VALUE_OPERAND, 0},
    {0, LOOM_LOW_PLACEMENT_VALUE_OPERAND, 1},
    {0, LOOM_LOW_PLACEMENT_VALUE_OPERAND, 2},
};
static const loom_low_placement_predicate_t kMatrixPredicates[] = {
    {0, 1, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 1},
    {0, 2, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
    {0, 2, 0, 1, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
    {1, 2, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
    {1, 2, 0, 1, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
    {0, 1, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
    {0, 2, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
    {0, 1, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
    {0, 2, 0, 1, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 3},
};
static const loom_low_placement_clause_t kMatrixClauses[] = {
    {0, 5, 1, LOOM_LOW_PLACEMENT_CLAUSE_ANY},
    {5, 2, 1, LOOM_LOW_PLACEMENT_CLAUSE_ALL},
    {7, 2, 1, LOOM_LOW_PLACEMENT_CLAUSE_ALL},
};
static const loom_low_placement_preference_t kInstructionPreferences[] = {
    {kMatrixValues, kMatrixPredicates, kMatrixClauses,
     IREE_ARRAYSIZE(kMatrixValues), IREE_ARRAYSIZE(kMatrixClauses)},
};

typedef struct loom_amdgpu_placement_binding_t {
  // Ordinal in the existing descriptor-set catalog.
  uint16_t descriptor_set_ordinal;
  // Qualified execution subgroup size.
  uint16_t subgroup_size;
  // Preference indexes in this actual descriptor set's ordinal space.
  const uint16_t* indices_by_descriptor;
} loom_amdgpu_placement_binding_t;

typedef struct loom_amdgpu_placement_binding_range_t {
  // First binding for one existing processor ordinal.
  uint16_t start;
  // Number of qualified descriptor/subgroup bindings.
  uint16_t count;
} loom_amdgpu_placement_binding_range_t;

#include "loom/target/arch/amdgpu/descriptors/placement_bindings.inl"

loom_low_placement_instruction_preferences_t
loom_amdgpu_placement_instruction_preferences(
    const loom_low_resolved_target_t* target) {
  const loom_amdgpu_processor_info_t* processor =
      loom_amdgpu_target_processor_from_resolved_target(target);
  const uint32_t subgroup_size =
      loom_low_resolved_target_bundle(target)->snapshot->subgroup_size;
  const loom_amdgpu_placement_binding_range_t range =
      kInstructionPreferenceRanges[processor->ordinal];
  for (uint16_t i = 0; i < range.count; ++i) {
    const loom_amdgpu_placement_binding_t* binding =
        &kInstructionPreferenceBindings[range.start + i];
    if (binding->descriptor_set_ordinal ==
            target->descriptor_set->descriptor_set_ordinal &&
        binding->subgroup_size == subgroup_size) {
      return (loom_low_placement_instruction_preferences_t){
          .indices_by_descriptor = binding->indices_by_descriptor,
          .preferences = kInstructionPreferences,
      };
    }
  }
  return (loom_low_placement_instruction_preferences_t){0};
}
