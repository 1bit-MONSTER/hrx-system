// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Dialect-provided SSA fact-refinement materializers.

#ifndef LOOM_TRANSFORMS_CLEANUP_FACT_REFINEMENT_POLICY_H_
#define LOOM_TRANSFORMS_CLEANUP_FACT_REFINEMENT_POLICY_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_builder_t loom_builder_t;

// Materializes a same-value SSA alias carrying |predicates|.
typedef iree_status_t (*loom_fact_refinement_materialize_value_fn_t)(
    loom_builder_t* builder, loom_value_id_t source,
    const loom_predicate_t* predicates, iree_host_size_t predicate_count,
    loom_type_t result_type, loom_location_id_t location,
    loom_value_id_t* out_value);

// One dialect's value-refinement support.
typedef struct loom_fact_refinement_value_provider_t {
  // Returns true when this provider owns |type|.
  bool (*supports)(loom_type_t type);
  // Builds the dialect's fact-identity operation.
  loom_fact_refinement_materialize_value_fn_t materialize;
} loom_fact_refinement_value_provider_t;

// Materializes a same-value carrier with |target_type|.
typedef iree_status_t (*loom_fact_refinement_materialize_carrier_fn_t)(
    loom_builder_t* builder, loom_value_id_t source, loom_type_t target_type,
    loom_location_id_t location, loom_value_id_t* out_value);

// One dialect's dependent-type carrier refinement support.
typedef struct loom_fact_refinement_carrier_provider_t {
  // Returns true when this provider can refine |source_type| to |target_type|.
  bool (*supports)(loom_type_t source_type, loom_type_t target_type);
  // Builds the dialect's carrier identity operation.
  loom_fact_refinement_materialize_carrier_fn_t materialize;
} loom_fact_refinement_carrier_provider_t;

// Compiler-composed refinement providers. Lookup is a bounded linear scan on
// the rare path where an exact predicate-bearing identity was discovered.
typedef struct loom_fact_refinement_policy_t {
  // Ordered value-refinement providers.
  struct {
    // Borrowed provider pointer array.
    const loom_fact_refinement_value_provider_t* const* values;
    // Number of provider pointers.
    iree_host_size_t count;
  } value_providers;
  // Ordered dependent-carrier providers.
  struct {
    // Borrowed provider pointer array.
    const loom_fact_refinement_carrier_provider_t* const* values;
    // Number of provider pointers.
    iree_host_size_t count;
  } carrier_providers;
} loom_fact_refinement_policy_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_CLEANUP_FACT_REFINEMENT_POLICY_H_
