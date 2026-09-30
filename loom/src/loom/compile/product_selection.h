// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Compiler product and root selection.

#ifndef LOOM_COMPILE_PRODUCT_SELECTION_H_
#define LOOM_COMPILE_PRODUCT_SELECTION_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/module.h"
#include "loom/target/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Language-level product selected from compile roots.
typedef enum loom_compile_product_e {
  LOOM_COMPILE_PRODUCT_INVALID = 0,
  LOOM_COMPILE_PRODUCT_KERNEL = 1,
  LOOM_COMPILE_PRODUCT_COMMAND = 2,
  LOOM_COMPILE_PRODUCT_MODULE = 3,
} loom_compile_product_t;

// Returns the stable public name of |product|.
iree_string_view_t loom_compile_product_name(loom_compile_product_t product);

// Resolved language-level product and roots for one compilation.
typedef struct loom_compile_product_selection_t {
  // Product inferred from or constrained by the selected roots.
  loom_compile_product_t product;
  // Selected roots, preserving explicit order/duplicates. Derived roots own
  // their names in the caller arena. Empty selects the entire module.
  iree_string_view_list_t roots;
  // Common target family authored on selected kernel roots, or NULL.
  const loom_target_fact_type_t* target_fact_type;
  // Number of selected kernel roots without an authored target.
  iree_host_size_t untargeted_kernel_count;
} loom_compile_product_selection_t;

// Selects one homogeneous product and its compile roots.
// Explicit roots are borrowed. Otherwise the constraint selects its complete
// default root set; an invalid constraint infers command, kernel, then module.
// Exclusions apply after inference. Derived names are copied into |arena|.
iree_status_t loom_compile_product_selection_resolve(
    const loom_module_t* module, iree_string_view_list_t explicit_roots,
    iree_string_view_list_t excluded_roots,
    loom_compile_product_t product_constraint, iree_arena_allocator_t* arena,
    loom_compile_product_selection_t* out_selection);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_COMPILE_PRODUCT_SELECTION_H_
