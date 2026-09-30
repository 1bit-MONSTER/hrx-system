// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable storage predicates and target-provided placement preferences.

#ifndef LOOM_CODEGEN_LOW_PLACEMENT_RECIPE_H_
#define LOOM_CODEGEN_LOW_PLACEMENT_RECIPE_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_low_placement_relation_kind_bits_e {
  // Unknown or uninitialized placement relation kind.
  LOOM_LOW_PLACEMENT_RELATION_UNKNOWN = 0,
  // Result and source unit ranges should occupy identical storage units.
  LOOM_LOW_PLACEMENT_RELATION_SAME_STORAGE = 1,
  // Result units should occupy a subrange of the source storage units.
  LOOM_LOW_PLACEMENT_RELATION_SUBRANGE = 2,
  // Result units should occupy a contiguous packed range of source values.
  LOOM_LOW_PLACEMENT_RELATION_CONTIGUOUS_PART = 3,
  // Result and source locations should differ under location_mask.
  LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION = 4,
  // Result and source unit ranges should occupy disjoint storage.
  LOOM_LOW_PLACEMENT_RELATION_DISJOINT_STORAGE = 5,
  // Values select the same candidate ordinal from distinct explicit
  // physical-register classes.
  LOOM_LOW_PLACEMENT_RELATION_SAME_REGISTER_ORDINAL = 6,
} loom_low_placement_relation_kind_bits_t;
typedef uint8_t loom_low_placement_relation_kind_t;

// Sentinel for pair affinities without a placement recipe. Nonzero indexes are
// stored as recipe index + 1 so zero-initialized affinity rows stay
// recipe-free.
#define LOOM_LOW_PLACEMENT_PAIR_RECIPE_NONE 0

typedef enum loom_low_placement_value_kind_bits_e {
  // Unknown or uninitialized value kind.
  LOOM_LOW_PLACEMENT_VALUE_UNKNOWN = 0,
  // Operation operand selected by index.
  LOOM_LOW_PLACEMENT_VALUE_OPERAND = 1,
  // Operation result selected by index.
  LOOM_LOW_PLACEMENT_VALUE_RESULT = 2,
} loom_low_placement_value_kind_bits_t;
typedef uint8_t loom_low_placement_value_kind_t;

// One real value in the operation span to which a recipe is bound. An
// instruction recipe uses operation zero; a scheduled pair uses zero and one.
typedef struct loom_low_placement_value_ref_t {
  // Zero-based operation index in the bound span.
  uint8_t operation_index;
  // Whether index selects an operand or result.
  loom_low_placement_value_kind_t kind;
  // Operand or result index within the selected operation.
  uint16_t index;
} loom_low_placement_value_ref_t;

// A predicate is true when the corresponding storage relation is violated.
// Value indexes refer to the containing preference's binding slots.
typedef struct loom_low_placement_predicate_t {
  // First binding slot participating in the relation.
  uint16_t result;
  // Second binding slot participating in the relation.
  uint16_t source;
  // Allocation-unit offset within the first value.
  uint16_t result_unit_offset;
  // Allocation-unit offset within the second value.
  uint16_t source_unit_offset;
  // Number of contiguous allocation units covered by the relation.
  uint16_t unit_count;
  // Location relation applied to the selected values.
  loom_low_placement_relation_kind_t kind;
  // Low location bits compared by DIFFERENT_MASKED_LOCATION.
  uint32_t location_mask;
} loom_low_placement_predicate_t;

typedef enum loom_low_placement_clause_kind_bits_e {
  // Charge the weight when any predicate is established.
  LOOM_LOW_PLACEMENT_CLAUSE_ANY = 0,
  // Charge the weight when every predicate is established.
  LOOM_LOW_PLACEMENT_CLAUSE_ALL = 1,
} loom_low_placement_clause_kind_bits_t;
typedef uint8_t loom_low_placement_clause_kind_t;

// One flat weighted clause. Unknown comparisons are not established, giving a
// conservative partial cost without searching possible completions.
typedef struct loom_low_placement_clause_t {
  // First predicate in the containing preference's predicate span.
  uint16_t predicate_start;
  // Number of contiguous predicate occurrences in this nonempty clause.
  uint16_t predicate_count;
  // Relative penalty charged when this clause is established.
  uint16_t weight;
  // Whether any or all predicates must be established.
  loom_low_placement_clause_kind_t kind;
} loom_low_placement_clause_t;

// One jointly applicable preference. All referenced values must be registers;
// a literal disables this preference, not other preferences on the operation.
// Recipes and their spans are immutable and outlive every allocation using
// them.
typedef struct loom_low_placement_preference_t {
  // Real operation values to bind to function-local value ordinals.
  const loom_low_placement_value_ref_t* values;
  // Predicate occurrences referenced by clauses.
  const loom_low_placement_predicate_t* predicates;
  // Flat clauses whose weighted costs are added.
  const loom_low_placement_clause_t* clauses;
  // Number of entries in values.
  uint16_t value_count;
  // Number of entries in clauses.
  uint16_t clause_count;
} loom_low_placement_preference_t;

// Optional instruction preferences indexed by the actual descriptor ordinal.
// Zero indexes are inert; nonzero indexes select preferences[index - 1].
typedef struct loom_low_placement_instruction_preferences_t {
  // Borrowed dense index, or NULL when no instruction has a preference.
  const uint16_t* indices_by_descriptor;
  // Borrowed immutable preferences referenced by the index.
  const loom_low_placement_preference_t* preferences;
} loom_low_placement_instruction_preferences_t;

// Target-provided placement recipe shared by compatible descriptor pairs.
// Each alternative is a conjunction of |preference_count| binary preferences.
// Every member has two values and one unit-weight, single-predicate clause.
// Alternatives are ordered by preference; placement selects the first one that
// is not structurally impossible for the concrete pair values.
typedef struct loom_low_placement_pair_recipe_t {
  // Borrowed binary preferences grouped contiguously by alternative.
  const loom_low_placement_preference_t* const* preferences;
  // Number of independent binary preferences in each alternative.
  uint16_t preference_count;
  // Number of ordered alternative relation conjunctions.
  uint16_t alternative_count;
  // Native packet count saved when one concrete use satisfies this recipe.
  uint16_t packet_savings;
} loom_low_placement_pair_recipe_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_PLACEMENT_RECIPE_H_
